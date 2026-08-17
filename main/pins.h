#pragma once

/*
 * Výchozí návrh pinů pro ESP32-C6.
 * Před zapojením ověř proti konkrétní vývojové desce.
 * Záměrně se nepoužívá:
 *   GPIO12/13 - USB Serial/JTAG
 *   GPIO16/17 - rezervováno pro budoucí UART/RS-485
 */

#define PIN_STATUS_LED      8
#define PIN_BOOT_BUTTON     9

#define PIN_BUTTON_1        0
#define PIN_BUTTON_2        1
#define PIN_BUTTON_3        2
#define PIN_BUTTON_4        3
#define PIN_BUTTON_5        6
#define PIN_BUTTON_6        7
#define PIN_BUTTON_7        10

#define PIN_ENCODER_A       11
#define PIN_ENCODER_B       18
#define PIN_ENCODER_BUTTON  19

#define PIN_FUTURE_UART_TX  16
#define PIN_FUTURE_UART_RX  17
