/**
 * @file hardware_pins.h
 * @brief Pin Definitions and Hardware Mapping for WEMOS LOLIN S2 Mini & CC1101
 *
 * ============================================================================
 * CRITICAL HARDWARE DESIGN NOTE (ESP32-S2 ADC1 vs ADC2 & Wi-Fi Coexistence):
 * ============================================================================
 * On the ESP32-S2 microcontroller, the Wi-Fi baseband and radio modem internally
 * arbitrate the ADC2 peripheral block (GPIO 11 through GPIO 20).
 *
 * If fast digital SPI clock or data signals (SCK, MOSI, MISO) are assigned to
 * any pin in the ADC2 domain (such as default FSPI GPIO 11/12):
 * 1. Digital RF/SPI switching noise leaks directly into the Wi-Fi receiver frontend.
 * 2. The ESP32-S2 fails the WPA2 4-way key exchange handshake with the router.
 * 3. The connection drops repeatedly with `WIFI_REASON_AUTH_EXPIRE (Reason 2)`.
 *
 * To guarantee 100% stable Wi-Fi performance and instant connection:
 * All SPI pins MUST be mapped to the isolated ADC1 domain (GPIO 1 to GPIO 10).
 *
 * ============================================================================
 * WIRING TABLE: LOLIN S2 Mini -> CC1101 SPI Transceiver (868 MHz)
 * ============================================================================
 * ESP32-S2 Pin    Domain     CC1101 Pin    Wire Color    Description
 * ----------------------------------------------------------------------------
 * 3V3             Power      VCC           Red           3.3V Power Supply
 * GND             Power      GND           Black         Ground Reference
 * GPIO 3          ADC1_CH2   CSN / SS      Orange        SPI Chip Select (Active LOW)
 * GPIO 5          ADC1_CH4   MOSI          Blue          SPI Master Out Slave In
 * GPIO 7          ADC1_CH6   SCK           Yellow        SPI Bus Clock (Up to 10 MHz)
 * GPIO 9          ADC1_CH8   MISO / GDO1   Green         SPI Master In Slave Out
 * ============================================================================
 */

#pragma once

#include <stdint.h>

/** @brief SPI Clock Pin on ESP32-S2 (ADC1 Domain, isolated from Wi-Fi) */
#define CC1101_PIN_SCK   7
#define PIN_SCK          CC1101_PIN_SCK

/** @brief SPI Master In Slave Out Pin on ESP32-S2 (ADC1 Domain) */
#define CC1101_PIN_MISO  9
#define PIN_MISO         CC1101_PIN_MISO

/** @brief SPI Master Out Slave In Pin on ESP32-S2 (ADC1 Domain) */
#define CC1101_PIN_MOSI  5
#define PIN_MOSI         CC1101_PIN_MOSI

/** @brief SPI Chip Select Pin on ESP32-S2 (ADC1 Domain) */
#define CC1101_PIN_CS    3
#define PIN_CS           CC1101_PIN_CS
