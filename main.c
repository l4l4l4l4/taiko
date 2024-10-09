#include <avr/io.h>
#include <util/delay.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include "usbdrv.h"

// Constants for piezo-to-key mapping
#define KEY_D 0x07 // HID code for 'd'
#define KEY_F 0x09 // HID code for 'f'
#define KEY_J 0x0D // HID code for 'j'
#define KEY_K 0x0E // HID code for 'k'

#define LED_PIN PD7 

// Threshold for registering a hit
#define THRESHOLD 300 

// Report buffer
static uchar reportBuffer[2];
static uchar idleRate;

const PROGMEM char usbHidReportDescriptor[USB_CFG_HID_REPORT_DESCRIPTOR_LENGTH] = { /* USB report descriptor */
    0x05, 0x01,                    // USAGE_PAGE (Generic Desktop)
    0x09, 0x06,                    // USAGE (Keyboard)
    0xa1, 0x01,                    // COLLECTION (Application)
    0x05, 0x07,                    //   USAGE_PAGE (Keyboard)
    0x19, 0xe0,                    //   USAGE_MINIMUM (Keyboard LeftControl)
    0x29, 0xe7,                    //   USAGE_MAXIMUM (Keyboard Right GUI)
    0x15, 0x00,                    //   LOGICAL_MINIMUM (0)
    0x25, 0x01,                    //   LOGICAL_MAXIMUM (1)
    0x75, 0x01,                    //   REPORT_SIZE (1)
    0x95, 0x08,                    //   REPORT_COUNT (8)
    0x81, 0x02,                    //   INPUT (Data,Var,Abs)
    0x95, 0x01,                    //   REPORT_COUNT (1)
    0x75, 0x08,                    //   REPORT_SIZE (8)
    0x25, 0x65,                    //   LOGICAL_MAXIMUM (101)
    0x19, 0x00,                    //   USAGE_MINIMUM (Reserved (no event indicated))
    0x29, 0x65,                    //   USAGE_MAXIMUM (Keyboard Application)
    0x81, 0x00,                    //   INPUT (Data,Ary,Abs)
    0xc0                           // END_COLLECTION
};
/* We use a simplifed keyboard report descriptor which does not support the
 * boot protocol. We don't allow setting status LEDs and we only allow one
 * simultaneous key press (except modifiers). We can therefore use short
 * 2 byte input reports.
 * The report descriptor has been created with usb.org's "HID Descriptor Tool"
 * which can be downloaded from http://www.usb.org/developers/hidpage/.
 * Redundant entries (such as LOGICAL_MINIMUM and USAGE_PAGE) have been omitted
 * for the second INPUT item.
 */


// ADC Initialization
void adcInit() {
    ADMUX = (1 << REFS0); // AVcc with external capacitor at AREF pin
    ADCSRA = (1 << ADEN) | (1 << ADPS2) | (1 << ADPS1); // Enable ADC, prescaler 64
}

uint8_t buildReport(){
    uint8_t highest_index = -1;
    uint16_t highest_value = 0;
    uint16_t adc_value = 0;

    // Will be used to check if there was a change
    uint8_t tmpBuf[sizeof(reportBuffer)];
    memcpy(tmpBuf, reportBuffer, sizeof(tmpBuf));

    reportBuffer[0] = 0;

    // Check PC0-PC3 using the ADC
    for(uint8_t i = 0; i < 4; i++){
        // Select ADC channel
        ADMUX = (ADMUX & 0xF0) | i; // Clear previous channel and set new one
        // Start a conversion
        ADCSRA |= (1 << ADSC);
        // Wait for the conversion to complete
        while(ADCSRA & (1 << ADSC));

        // Read ADC value
        adc_value = ADC;

        // Check if this is the highest over the threshold
        if(adc_value > THRESHOLD && adc_value > highest_value){
            highest_value = adc_value;
            highest_index = i;
        }
    }

    // Set key according to highest_index
    switch(highest_index){
        case 0: reportBuffer[1] = KEY_D; break;
        case 1: reportBuffer[1] = KEY_F;  break; 
        case 2: reportBuffer[1] = KEY_J; break;
        case 3: reportBuffer[1] = KEY_K; break;
        default: reportBuffer[1] = 0; break; // No valid key pressed
    }

    // They are the same (no change)
    if(memcmp(tmpBuf, reportBuffer, sizeof(tmpBuf)) == 0){
        return 0;
    }

    PORTD ^= (1 << LED_PIN);
    return 1;
}


usbMsgLen_t usbFunctionSetup(uint8_t data[8]){
    usbRequest_t *rq = (void *) data;

    if((rq->bmRequestType & USBRQ_TYPE_MASK) == USBRQ_TYPE_MASK){
        switch(rq->bRequest){
        
            case USBRQ_HID_GET_REPORT:
                // Treat same as our interrupt IN (send keys)
                buildReport();
                usbMsgPtr = reportBuffer;
                return sizeof(reportBuffer);

            case USBRQ_HID_SET_REPORT:
                // Should only by 1 byte for the LEDs
                if(rq->wLength.word == 1){
                    // Go to usbFunctionWrite() instead
                    return USB_NO_MSG;
                }

                return 0;

            // Send or change Idle rate when commanded
            case USBRQ_HID_GET_IDLE:
                usbMsgPtr = &idleRate;
                return 1;

            case USBRQ_HID_SET_IDLE:
                idleRate = rq->wValue.bytes[1];
                return 0;
        }
    }

    // By default, return no data back
    return 0;
}

void    usbEventResetReady(void)
{
    cli();
    sei();
}


int main(){
    // Enable watchdog in case of an unrecoverable error
    // Reset after 2 seconds
    wdt_enable(WDTO_2S);

    // LED and ADC init 
    DDRD |= (1 << LED_PIN);
    adcInit();

    // Allow VUSB to initialize itself
    usbInit();
    sei();

    // Re-enumerate device
    usbDeviceDisconnect();
    uchar i;
    for(i=0;i<20;i++){  /* 300 ms disconnect */
        _delay_ms(15);
    }
    usbDeviceConnect();

    while(1){
        // Reset the watchdog reset countdown
        wdt_reset();
        usbPoll();
        uint8_t change = buildReport();

        // 0 is an indefinite idle
        if(idleRate != 0) {
            for(int i = 0; i < idleRate; i++){
                _delay_ms(4);
            }
        }

        // Interrupt IN request, and there is new data to report
        if(usbInterruptIsReady() && change == 1){
            // Send over the HID data
            usbSetInterrupt(reportBuffer, sizeof(reportBuffer));
        }
    }

    return 0;
}
