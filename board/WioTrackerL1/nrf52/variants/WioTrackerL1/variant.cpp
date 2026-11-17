// Pin-Mapping Seeed Wio Tracker L1 / L1 Pro
// Arduino-Pin Dx -> nRF-Pin (Port*32 + Pin), Werte aus dem Meshtastic-Variant
// "seeed_wio_tracker_L1" (variant.cpp). Kommentare dort sind teils widerspruechlich,
// massgeblich sind die Zahlen.
#include "variant.h"
#include "wiring_constants.h"
#include "wiring_digital.h"
#include "nrf.h"

const uint32_t g_ADigitalPinMap[] = {
    41,  // D0  P1.09 GNSS Wakeup/Standby
     7,  // D1  P0.07 LoRa DIO1
    39,  // D2  P1.07 LoRa Reset
    42,  // D3  P1.10 LoRa Busy
    46,  // D4  P1.14 LoRa CS
    40,  // D5  P1.08 LoRa RF-Switch (RXEN)
    27,  // D6  P0.27 UART TX (MCU -> GNSS)
    26,  // D7  P0.26 UART RX (GNSS -> MCU)
    30,  // D8  P0.30 SPI SCK
     3,  // D9  P0.03 SPI MISO
    28,  // D10 P0.28 SPI MOSI
    33,  // D11 P1.01 User-LED
    32,  // D12 P1.00 Buzzer
     8,  // D13 P0.08 User-Taste
     6,  // D14 P0.06 OLED SDA
     5,  // D15 P0.05 OLED SCL
    31,  // D16 P0.31 VBAT ADC (AIN7)
    43,  // D17 P1.11 Grove SDA
    44,  // D18 P1.12 Grove SCL
    21,  // D19 P0.21 QSPI SCK
    25,  // D20 P0.25 QSPI CS
    20,  // D21 P0.20 QSPI IO0
    24,  // D22 P0.24 QSPI IO1
    22,  // D23 P0.22 QSPI IO2
    23,  // D24 P0.23 QSPI IO3
    36,  // D25 P1.04 Joystick hoch
    12,  // D26 P0.12 Joystick runter
    11,  // D27 P0.11 Joystick links
    35,  // D28 P1.03 Joystick rechts
    37,  // D29 P1.05 Joystick druecken
     4,  // D30 P0.04 Batterie-Teiler ein (BAT_CTL)
};

void initVariant() {
  // externen Flash abwaehlen
  pinMode(PIN_QSPI_CS, OUTPUT);
  digitalWrite(PIN_QSPI_CS, HIGH);
  // Batteriespannungs-Teiler einschalten
  pinMode(PIN_VBAT_ENABLE, OUTPUT);
  digitalWrite(PIN_VBAT_ENABLE, HIGH);
  // LED und Buzzer aus
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, !LED_STATE_ON);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
}
