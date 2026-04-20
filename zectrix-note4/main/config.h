// Pin map for ZecTrix Note 4 (4.2" e-paper, project codename "AI便利贴").
// Copied from the upstream ZecTrix SDK (main/boards/zectrix-s3-epaper-4.2/config.h).
// If you move this firmware to a different PCB revision, just swap the GPIOs.

#pragma once
#include <driver/gpio.h>

// ---- Display (SSD2683 panel, 400x300 monochrome) ----
#define EPD_SPI_HOST     SPI3_HOST
#define EPD_DC_PIN       GPIO_NUM_10
#define EPD_CS_PIN       GPIO_NUM_11
#define EPD_SCK_PIN      GPIO_NUM_12
#define EPD_MOSI_PIN     GPIO_NUM_13
#define EPD_RST_PIN      GPIO_NUM_9
#define EPD_BUSY_PIN     GPIO_NUM_8
#define EPD_PWR_PIN      GPIO_NUM_6
#define EPD_WIDTH        400
#define EPD_HEIGHT       300

// ---- Audio codec (ES8311 over I2C + I2S). Used for speaker beeps ----
#define AUDIO_I2S_MCLK   GPIO_NUM_14
#define AUDIO_I2S_WS     GPIO_NUM_38
#define AUDIO_I2S_BCLK   GPIO_NUM_15
#define AUDIO_I2S_DIN    GPIO_NUM_16
#define AUDIO_I2S_DOUT   GPIO_NUM_45
#define AUDIO_PA_PIN     GPIO_NUM_46
#define AUDIO_AMP_PIN    GPIO_NUM_46     // same as PA
#define AUDIO_PWR_PIN    GPIO_NUM_42
#define AUDIO_I2C_SDA    GPIO_NUM_47
#define AUDIO_I2C_SCL    GPIO_NUM_48
#define AUDIO_ES8311_ADDR  0x18          // ES8311 default 7-bit I2C addr

// ---- Buttons ----
// BOOT and CONFIRM share GPIO 0 — the front voice/confirm button.
#define BTN_CONFIRM_GPIO GPIO_NUM_0
#define BTN_UP_GPIO      GPIO_NUM_39
// DOWN and VBAT_PWR share GPIO 18 — long-press this for power off.
#define BTN_DOWN_GPIO    GPIO_NUM_18

// ---- Power rails ----
#define VBAT_PWR_PIN     GPIO_NUM_17

// ---- Charge status ----
#define CHARGE_DETECT_GPIO   GPIO_NUM_2
#define CHARGE_FULL_GPIO     GPIO_NUM_1
#define CHARGE_DETECT_CHARGING_LEVEL 0   // 0 == charging

// ---- RTC (PCF8563 over I2C, shared bus with audio codec) ----
#define RTC_INT_GPIO     GPIO_NUM_5
#define RTC_I2C_ADDR     0x51

// ---- NFC (GT23SC6699) — not used in buddy firmware but pins reserved ----
#define NFC_I2C_ADDR     0x55
#define NFC_FD_GPIO      GPIO_NUM_7
#define NFC_PWR_GPIO     GPIO_NUM_21
