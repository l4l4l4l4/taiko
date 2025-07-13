#![no_std]
#![no_main]

use core::sync::atomic::{AtomicBool, Ordering};

use core::cell::RefCell;
use defmt::*;
use embassy_executor::Spawner;
use embassy_futures::join::join;
use embassy_stm32::adc::{Adc, AdcChannel, AnyAdcChannel};
use embassy_stm32::gpio::{AnyPin, Level, Output, Speed};
use embassy_stm32::time::Hertz;
use embassy_stm32::usb::Driver;
use embassy_stm32::{Config, Peri, bind_interrupts, peripherals, usb};
use embassy_sync::blocking_mutex::raw::ThreadModeRawMutex;
use embassy_sync::mutex::Mutex;
use embassy_time::{Instant, Timer};
use embassy_usb::class::hid::{HidReaderWriter, ReportId, RequestHandler, State};
use embassy_usb::control::OutResponse;
use embassy_usb::{Builder, Handler};
use usbd_hid::descriptor::{KeyboardReport, SerializedDescriptor};
use {defmt_rtt as _, panic_probe as _};

const THRESHOLD_HIT: u16 = 650;
const SAMPLE_COUNT: u16 = 5;
const COOLDOWN_TIME_MS: u64 = 25;
const BASELINE_SAMPLE_COUNT: u16 = 5;
const KEYCODES: [u8; 4] = [0x07, 0x09, 0x0d, 0x0e];

bind_interrupts!(struct Irqs {
    OTG_FS => usb::InterruptHandler<peripherals::USB_OTG_FS>;
});

fn convert_to_millivolts(sample: u16, vrefint_sample: u16) -> u16 {
    // From http://www.st.com/resource/en/datasheet/DM00071990.pdf
    // 6.3.24 Reference voltage
    const VREFINT_MV: u32 = 1210; // mV

    (u32::from(sample) * VREFINT_MV / u32::from(vrefint_sample)) as u16
}

fn abs(value: i16) -> u16 {
    if value < 0 {
        (-value) as u16
    } else {
        value as u16
    }
}

#[embassy_executor::main]
async fn main(spawner: Spawner) {
    let mut config = Config::default();
    {
        use embassy_stm32::rcc::*;
        config.rcc.hsi = false;
        config.rcc.hse = Some(Hse {
            freq: Hertz(25_000_000),
            mode: HseMode::Oscillator,
        });
        config.rcc.pll_src = PllSource::HSE;
        config.rcc.pll = Some(Pll {
            prediv: PllPreDiv::DIV25,
            mul: PllMul::MUL192,
            divp: Some(PllPDiv::DIV2), // 25mhz / 25 * 192 / 2 = 96Mhz.
            divq: Some(PllQDiv::DIV4), // 25mhz / 25 * 192 / 4 = 48Mhz.
            divr: None,
        });
        config.rcc.ahb_pre = AHBPrescaler::DIV1;
        config.rcc.apb1_pre = APBPrescaler::DIV2;
        config.rcc.apb2_pre = APBPrescaler::DIV1;
        config.rcc.sys = Sysclk::PLL1_P;
        config.rcc.mux.clk48sel = mux::Clk48sel::PLL1_Q;
    }
    let p = embassy_stm32::init(config);

    let mut channels: [AnyAdcChannel<peripherals::ADC1>; 4] = [
        p.PA0.degrade_adc(),
        p.PA1.degrade_adc(),
        p.PA2.degrade_adc(),
        p.PA3.degrade_adc(),
    ];

    let mut led = Output::new(p.PC13, Level::High, Speed::Low);

    let mut adc = Adc::new(p.ADC1);
    let mut vrefint = adc.enable_vrefint();
    let vrefint_sample = adc.blocking_read(&mut vrefint);

    // Create the driver, from the HAL.
    let mut ep_out_buffer = [0u8; 256];
    let mut config = embassy_stm32::usb::Config::default();
    config.vbus_detection = false;

    let driver = Driver::new_fs(
        p.USB_OTG_FS,
        Irqs,
        p.PA12,
        p.PA11,
        &mut ep_out_buffer,
        config,
    );

    // Create embassy-usb Config
    let mut config = embassy_usb::Config::new(0xc0de, 0xcafe);
    config.manufacturer = Some("lalala.ing");
    config.product = Some("taiko");
    config.serial_number = Some("1337");
    config.max_power = 100;
    config.max_packet_size_0 = 64;

    let mut config_descriptor = [0; 256];
    let mut bos_descriptor = [0; 256];
    // Microsoft OS descriptor??
    let mut msos_descriptor = [0; 256];
    let mut control_buf = [0; 64];

    let mut request_handler = MyRequestHandler {};
    let mut device_handler = MyDeviceHandler::new();

    let mut state = State::new();
    let mut builder = Builder::new(
        driver,
        config,
        &mut config_descriptor,
        &mut bos_descriptor,
        &mut msos_descriptor,
        &mut control_buf,
    );

    builder.handler(&mut device_handler);

    // Create classes on the builder.
    let config = embassy_usb::class::hid::Config {
        report_descriptor: KeyboardReport::desc(),
        request_handler: None,
        poll_ms: 60,
        max_packet_size: 8,
    };

    let hid = HidReaderWriter::<_, 1, 8>::new(&mut builder, &mut state, config);
    let mut baseline: [u16; 4] = [0; 4];
    for _ in 0..BASELINE_SAMPLE_COUNT {
        for channel_number in 0..4 {
            baseline[channel_number] += convert_to_millivolts(
                adc.blocking_read(&mut channels[channel_number]),
                vrefint_sample,
            );
        }
        Timer::after_millis(500).await;
        led.toggle();
    }

    for channel_number in 0..4 {
        baseline[channel_number] /= BASELINE_SAMPLE_COUNT;
    }
    info!(
        "baseline: {}, {}, {}, {}",
        baseline[0], baseline[1], baseline[2], baseline[3]
    );
    // USB things
    let mut usb = builder.build();
    let usb_fut = usb.run();
    let (reader, mut writer) = hid.split();

    // Do stuff with the class!
    let in_fut = async {
        Timer::after_millis(1000).await;
        let mut cooldown_timestamps: [Instant; 4] = [Instant::now(); 4];
        let mut report_raw: [u8; 6] = [0; 6];
        loop {
            let mut report_changed: bool = false;
            let mut array_of_deviations_averaged: [u16; 4] = [0; 4];
            //read values, get potential hits
            for channel_number in 0..4 {
                let mut deviation_averaged = 0;
                for sample_number in 0..SAMPLE_COUNT {
                    let value = convert_to_millivolts(
                        adc.blocking_read(&mut channels[channel_number]),
                        vrefint_sample,
                    );
                    let deviation = abs(baseline[channel_number] as i16 - value as i16);
                    deviation_averaged += deviation;
                }
                deviation_averaged /= SAMPLE_COUNT;
                array_of_deviations_averaged[channel_number] = deviation_averaged;
                let cooldown_ended: bool = Instant::now()
                    .duration_since(cooldown_timestamps[channel_number])
                    .as_millis()
                    > COOLDOWN_TIME_MS;
                if cooldown_ended {
                    if report_raw[channel_number] == 0 {
                        if deviation_averaged > THRESHOLD_HIT {
                            report_raw[channel_number] = KEYCODES[channel_number];
                            cooldown_timestamps[channel_number] = Instant::now();
                            report_changed = true;
                            info!(
                                "channel {} deviation {}",
                                channel_number, deviation_averaged
                            );
                        }
                    } else {
                        report_raw[channel_number] = 0;
                        report_changed = true;
                    }
                }
            }
            if !report_changed {
                continue;
            } else {
                //check if kat/dom triggered at the same time.
                if report_raw[0] > 0 && report_raw[1] > 0 {
                    if array_of_deviations_averaged[0] < array_of_deviations_averaged[1] {
                        report_raw[0] = 0;
                    }
                    if array_of_deviations_averaged[1] < array_of_deviations_averaged[0] {
                        report_raw[1] = 0;
                    }
                }
                if report_raw[2] > 0 && report_raw[3] > 0 {
                    if array_of_deviations_averaged[2] < array_of_deviations_averaged[3] {
                        report_raw[2] = 0;
                    }
                    if array_of_deviations_averaged[3] < array_of_deviations_averaged[2] {
                        report_raw[3] = 0;
                    }
                }
            }
            let report = KeyboardReport {
                keycodes: report_raw,
                leds: 0,
                modifier: 0,
                reserved: 0,
            };
            // Send the report.
            match writer.write_serialize(&report).await {
                Ok(()) => {}
                Err(e) => warn!("Failed to send report: {:?}", e),
            };
            Timer::after_millis(2).await; // for out_fut and usb_fut to do their thing
        }
    };

    let out_fut = async {
        reader.run(false, &mut request_handler).await;
    };

    // Run everything concurrently.
    // If we had made everything `'static` above instead, we could do this using separate tasks instead.
    join(usb_fut, join(in_fut, out_fut)).await;
}

struct MyRequestHandler {}

impl RequestHandler for MyRequestHandler {
    fn get_report(&mut self, id: ReportId, _buf: &mut [u8]) -> Option<usize> {
        info!("Get report for {:?}", id);
        None
    }

    fn set_report(&mut self, id: ReportId, data: &[u8]) -> OutResponse {
        info!("Set report for {:?}: {=[u8]}", id, data);
        OutResponse::Accepted
    }

    fn set_idle_ms(&mut self, id: Option<ReportId>, dur: u32) {
        info!("Set idle rate for {:?} to {:?}", id, dur);
    }

    fn get_idle_ms(&mut self, id: Option<ReportId>) -> Option<u32> {
        info!("Get idle rate for {:?}", id);
        None
    }
}

struct MyDeviceHandler {
    configured: AtomicBool,
}

impl MyDeviceHandler {
    fn new() -> Self {
        MyDeviceHandler {
            configured: AtomicBool::new(false),
        }
    }
}

impl Handler for MyDeviceHandler {
    fn enabled(&mut self, enabled: bool) {
        self.configured.store(false, Ordering::Relaxed);
        if enabled {
            info!("Device enabled");
        } else {
            info!("Device disabled");
        }
    }

    fn reset(&mut self) {
        self.configured.store(false, Ordering::Relaxed);
        info!("Bus reset, the Vbus current limit is 100mA");
    }

    fn addressed(&mut self, addr: u8) {
        self.configured.store(false, Ordering::Relaxed);
        info!("USB address set to: {}", addr);
    }

    fn configured(&mut self, configured: bool) {
        self.configured.store(configured, Ordering::Relaxed);
        if configured {
            info!(
                "Device configured, it may now draw up to the configured current limit from Vbus."
            )
        } else {
            info!("Device is no longer configured, the Vbus current limit is 100mA.");
        }
    }
}
