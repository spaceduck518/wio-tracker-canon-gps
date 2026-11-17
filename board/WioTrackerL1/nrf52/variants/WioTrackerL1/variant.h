#ifndef _WIO_TRACKER_L1_H_
#define _WIO_TRACKER_L1_H_

#define VARIANT_MCK (64000000ul)

// Ob ein 32-kHz-Quarz bestueckt ist, ist nicht dokumentiert -> RC-Oszillator (geht immer)
#define USE_LFRC

#include "WVariant.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PINS_COUNT          (31)
#define NUM_DIGITAL_PINS    (31)
#define NUM_ANALOG_INPUTS   (8)
#define NUM_ANALOG_OUTPUTS  (0)

static const uint8_t D0 = 0, D1 = 1, D2 = 2, D3 = 3, D4 = 4, D5 = 5, D6 = 6, D7 = 7;
static const uint8_t D8 = 8, D9 = 9, D10 = 10, D11 = 11, D12 = 12, D13 = 13, D14 = 14, D15 = 15;
static const uint8_t D16 = 16;

// LED / Taste / Buzzer
#define LED_BUILTIN         (11)
#define PIN_LED             LED_BUILTIN
#define LED_RED             LED_BUILTIN
#define LED_BLUE            LED_BUILTIN
#define LED_STATE_ON        (1)
#define PIN_NEOPIXEL        (PINS_COUNT)
#define NEOPIXEL_NUM        (0)
#define PIN_BUTTON1         (13)    // aktiv LOW
#define PIN_BUZZER          (12)

// Joystick (nur L1 Pro)
#define PIN_JOY_UP          (25)
#define PIN_JOY_DOWN        (26)
#define PIN_JOY_LEFT        (27)
#define PIN_JOY_RIGHT       (28)
#define PIN_JOY_PRESS       (29)

// GNSS (Quectel L76K)
#define PIN_GNSS_WAKEUP     (0)
#define PIN_SERIAL1_RX      (7)
#define PIN_SERIAL1_TX      (6)

// Batterie
#define PIN_VBAT            (16)
#define PIN_VBAT_ENABLE     (30)    // HIGH = Spannungsteiler aktiv
#define VBAT_DIVIDER        (2.0f)
#define PIN_A0              (16)
static const uint8_t A0 = PIN_A0;
#define ADC_RESOLUTION      (12)

// LoRa SX1262 (hier nur zum Schlafenlegen)
#define PIN_LORA_DIO1       (1)
#define PIN_LORA_RESET      (2)
#define PIN_LORA_BUSY       (3)
#define PIN_LORA_CS         (4)
#define PIN_LORA_RXEN       (5)

// SPI
#define SPI_INTERFACES_COUNT (1)
#define PIN_SPI_MISO        (9)
#define PIN_SPI_MOSI        (10)
#define PIN_SPI_SCK         (8)
static const uint8_t SS   = PIN_LORA_CS;
static const uint8_t MOSI = PIN_SPI_MOSI;
static const uint8_t MISO = PIN_SPI_MISO;
static const uint8_t SCK  = PIN_SPI_SCK;

// I2C: Wire = OLED, Wire1 = Grove
#define WIRE_INTERFACES_COUNT (2)
#define PIN_WIRE_SDA        (14)
#define PIN_WIRE_SCL        (15)
#define PIN_WIRE1_SDA       (17)
#define PIN_WIRE1_SCL       (18)
static const uint8_t SDA = PIN_WIRE_SDA;
static const uint8_t SCL = PIN_WIRE_SCL;

// QSPI-Flash
#define PIN_QSPI_SCK        (19)
#define PIN_QSPI_CS         (20)
#define PIN_QSPI_IO0        (21)
#define PIN_QSPI_IO1        (22)
#define PIN_QSPI_IO2        (23)
#define PIN_QSPI_IO3        (24)
#define EXTERNAL_FLASH_DEVICES  P25Q16H
#define EXTERNAL_FLASH_USE_QSPI

#ifdef __cplusplus
}
#endif

#endif
