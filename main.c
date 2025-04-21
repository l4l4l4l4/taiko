#include "usbdrv.h"
#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/wdt.h>
#include <stdio.h>
#include <stdlib.h>
#include <util/delay.h>

#define UART_DEBUG // comment this out to disable.
#define UART_SPEED 48000

// Constants for piezo-to-key mapping
#define KEY_D 0x07 // HID code for 'd'
#define KEY_F 0x09 // HID code for 'f'
#define KEY_J 0x0D // HID code for 'j'
#define KEY_K 0x0E // HID code for 'k'

#define LED_PIN PD7

// Threshold for registering a hit
#define THRESHOLD 10

// USB variables
static uchar reportBuffer[2];
static uchar idleRate;

// hit calculation variables
static uint8_t cooldownBits[4];
static uint8_t lastStepMemory[4];
static uint16_t baselineValue;

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

int readAdc(char chan) {
  ADMUX = (1 << REFS0) | (chan & 0x0f); // select ref (REFS0) and channel
  ADCSRA |= (1 << ADSC);                // start the conversion
  while (ADCSRA & (1 << ADSC))
    ;         // wait for end of conversion
  return ADC; // Return 16 Bit Reading Register
}

uint16_t getBaselineValue() {
  uint16_t sum = 0;
  uint16_t adc_value = 0;

  for (uint8_t i = 0; i < 4; i++) {
    sum += readAdc(i);
  }

  return sum / 4;
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

  PORTD ^= (1 << LED_PIN); // blink
}

uint8_t buildReport() {
  uint16_t adc_value = 0;
  uint16_t deviation = 0;

  // Will be used to check if there was a change
  uint8_t tmpBuf[sizeof(reportBuffer)];
  memcpy(tmpBuf, reportBuffer, sizeof(tmpBuf));
  memset(reportBuffer, 0, sizeof(reportBuffer));

  for (uint8_t i = 0; i < 4; i++) {
    adc_value = readAdc(i);

    deviation = abs(baselineValue - adc_value);

    // Check if the deviation is big enough and the cooldown bit is not set
    if (deviation > THRESHOLD)
      addToReport(i);

#ifdef UART_DEBUG
    printf(" %d", deviation);
#endif /* ifdef UART_DEBUG */
  }

#ifdef UART_DEBUG
  printf("|%d|", baselineValue);
  printf("\n");
#endif /* ifdef UART_DEBUG */
  // They are the same (no change)
  if (memcmp(tmpBuf, reportBuffer, sizeof(tmpBuf)) == 0) {
    return 0;
  }

  return 1;
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

FILE uart_output = FDEV_SETUP_STREAM(uart_putchar, NULL, _FDEV_SETUP_WRITE);

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
#endif /* ifdef UART_DEBUG */

  baselineValue = getBaselineValue();
  memset(cooldownBits, 0, sizeof(cooldownBits));
  memset(lastStepMemory, 0, sizeof(lastStepMemory));

  while (1) {
    // Reset the watchdog reset countdown
    wdt_reset();
    usbPoll();
    uint8_t change = buildReport();

    // 0 is an indefinite idle
    if (idleRate != 0) {
      for (int i = 0; i < idleRate; i++) {
        _delay_ms(4);
      }
    }

    // Interrupt IN request, and there is new data to report
    if (usbInterruptIsReady() && change == 1) {
      // Send over the HID data
      usbSetInterrupt(reportBuffer, sizeof(reportBuffer));
#ifdef UART_DEBUG
      printf("REPORT %d %d\n", reportBuffer[0], reportBuffer[1]);
#endif /* ifdef UART_DEBUG */
    }
  }

  return 0;
}
