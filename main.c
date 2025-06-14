#include "usbdrv.h"
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/wdt.h>
#include <stdio.h>
#include <stdlib.h>
#include <util/delay.h>

// #define UART_DEBUG // comment this out to disable.
// #define UART_SPEED 48000
FILE uart_output;

// Constants for piezo-to-key mapping
#define KEY_D 0x07 // HID code for 'd'
#define KEY_F 0x09 // HID code for 'f'
#define KEY_J 0x0D // HID code for 'j'
#define KEY_K 0x0E // HID code for 'k'

#define LED_PIN PD7

// Threshold for registering a hit
#define THRESHOLD 12

// USB variables
static uchar reportBuffer[2];
static uchar idleRate;

// hit calculation variables
static uint8_t cooldownBits[4];
static uint16_t last_step_memory[4];

// state stuff
static uint8_t state;
#define STATE_WAIT 0
#define STATE_SEND_KEY 1
#define STATE_SEND_BLANK 2

const PROGMEM char
    usbHidReportDescriptor[USB_CFG_HID_REPORT_DESCRIPTOR_LENGTH] = {
        /* USB report descriptor */
        0x05, 0x01, // USAGE_PAGE (Generic Desktop)
        0x09, 0x06, // USAGE (Keyboard)
        0xa1, 0x01, // COLLECTION (Application)
        0x05, 0x07, //   USAGE_PAGE (Key Codes)
        0x95, 0x02, //   REPORT_COUNT (2)
        0x75, 0x08, //   REPORT_SIZE (8)
        0x15, 0x00, //   LOGICAL_MINIMUM (0)
        0x25, 0x65, //   LOGICAL_MAXIMUM (101)
        0x19, 0x00, //   USAGE_MINIMUM (Reserved (no event indicated))
        0x29, 0x65, //   USAGE_MAXIMUM (Keyboard Application)
        0x81, 0x00, //   INPUT (Data,Ary,Abs)
        0xc0        // END_COLLECTION
};

// ADC Initialization
void adcInit() {
  ADMUX = (1 << REFS0); // AVcc with external capacitor at AREF pin
  ADCSRA =
      (1 << ADEN) | (1 << ADPS2) | (1 << ADPS1); // Enable ADC, prescaler 64
}

int adcRead(char chan) {
  ADMUX = (1 << REFS0) | (chan & 0x0f); // select ref (REFS0) and channel
  ADCSRA |= (1 << ADSC);                // start the conversion
  while (ADCSRA & (1 << ADSC))
    ;         // wait
  return ADC; // Return 16 Bit Register
}

void addToReport(uint8_t channel_index) {
  uint8_t report_index = 0;
  if (reportBuffer[0] > 0)
    report_index = 1;

  switch (channel_index) {
  case 0:
    reportBuffer[report_index] = KEY_D;
    break;
  case 1:
    reportBuffer[report_index] = KEY_F;
    break;
  case 2:
    reportBuffer[report_index] = KEY_J;
    break;
  case 3:
    reportBuffer[report_index] = KEY_K;
    break;
  }
}

void buildReport() {
  uint16_t adc_value;
  uint16_t deviation;

  for (uint8_t i = 0; i < 4; i++) {
    adc_value = adcRead(i);
    deviation = abs(last_step_memory[i] - adc_value);
    last_step_memory[i] = adc_value;

    if (deviation > THRESHOLD) {
      addToReport(i);
      state = STATE_SEND_KEY;
    }

#ifdef UART_DEBUG
    printf("  |%d %d %d|", adc_value, last_step_memory[i], deviation);
#endif /* ifdef UART_DEBUG */
  }

#ifdef UART_DEBUG
  printf("\n");
#endif /* ifdef UART_DEBUG */
}

usbMsgLen_t usbFunctionSetup(uint8_t data[8]) {
  usbRequest_t *rq = (void *)data;

  if ((rq->bmRequestType & USBRQ_TYPE_MASK) == USBRQ_TYPE_MASK) {
    switch (rq->bRequest) {

    case USBRQ_HID_GET_REPORT:
      // Treat same as our interrupt IN (send keys)
      memset(reportBuffer, 0, sizeof(reportBuffer));
      usbMsgPtr = reportBuffer;
      return sizeof(reportBuffer);

    case USBRQ_HID_SET_REPORT:
      // Should only by 1 byte for the LEDs
      if (rq->wLength.word == 1) {
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

void usbEventResetReady(void) {
  cli();
  sei();
}

// UART =============================

void uartInit(uint32_t baud) {
  uint16_t ubrr = F_CPU / 16 / baud - 1;
  UBRR0H = (ubrr >> 8);
  UBRR0L = ubrr;

  UCSR0B = (1 << RXEN0) | (1 << TXEN0);
  UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}

void uartTransmit(unsigned char data) {
  while (!(UCSR0A & (1 << UDRE0)))
    ;
  UDR0 = data;
}

int uart_putchar(char c, FILE *stream) {
  if (c == '\n') {
    uart_putchar('\r', stream);
  }
  uartTransmit(c);
  return 0;
}

// ==================================

int main() {
  // Enable watchdog in case of an unrecoverable error
  // Reset after 2 seconds
  wdt_enable(WDTO_2S);

  // LED and ADC init
  DDRD |= (1 << LED_PIN);
  DDRC = 0x00;
  adcInit();

  // Allow VUSB to initialize itself
  usbInit();
  sei();

  // Re-enumerate device
  usbDeviceDisconnect();
  uchar i;
  for (i = 0; i < 20; i++) { /* 300 ms disconnect */
    _delay_ms(15);
  }
  usbDeviceConnect();

#ifdef UART_DEBUG
  uartInit(UART_SPEED);
  stdout = &uart_output;
  uart_output = FDEV_SETUP_STREAM(uart_putchar, NULL, _FDEV_SETUP_WRITE);
#endif /* ifdef UART_DEBUG */

  memset(cooldownBits, 0, sizeof(cooldownBits));
  memset(last_step_memory, 1024 / 2, sizeof(last_step_memory));
  state = STATE_WAIT;

  while (1) {
    wdt_reset();
    usbPoll();

    // 0 is an indefinite idle
    if (idleRate != 0) {
      for (int i = 0; i < idleRate; i++) {
        _delay_ms(4);
      }
    }

    if (state == STATE_WAIT)
      buildReport();
    else if (state == STATE_SEND_KEY && usbInterruptIsReady()) {
      usbSetInterrupt(reportBuffer, sizeof(reportBuffer));
      state = STATE_SEND_BLANK;
    } else if (state == STATE_SEND_BLANK && usbInterruptIsReady()) {
      memset(reportBuffer, 0, sizeof(reportBuffer));
      usbSetInterrupt(reportBuffer, sizeof(reportBuffer));
      state = STATE_WAIT;
      PORTD ^= (1 << LED_PIN); // blink
    }

#ifdef UART_DEBUG
    printf("STATE %d\n", state);
#endif /* ifdef UART_DEBUG */
  }
  return 0;
}
