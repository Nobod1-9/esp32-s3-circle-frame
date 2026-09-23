#pragma once
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_rgb.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/task.h"

#include "TCA9554PWR.h"

#define LCD_CLK_PIN   2
#define LCD_MOSI_PIN  1 
#define LCD_Backlight_PIN   6 
// Backlight   ledChannel：PWM Channe 
#define PWM_Channel     1       // PWM Channel   
#define Frequency       20000   // PWM frequencyconst        
#define Resolution      10       // PWM resolution ratio     MAX:13
#define Dutyfactor      500     // PWM Dutyfactor        
#define Backlight_MAX   100   

extern uint8_t LCD_Backlight;

#define ESP_PANEL_LCD_WIDTH                       (480)
#define ESP_PANEL_LCD_HEIGHT                      (480)
// 6 MHz / (548 horizontal clocks × 508 vertical lines) ≈ 21.6 FPS.
#define ESP_PANEL_LCD_RGB_TIMING_FREQ_HZ          (6 * 1000 * 1000)
#define ESP_PANEL_LCD_RGB_TIMING_HPW              (8)
#define ESP_PANEL_LCD_RGB_TIMING_HBP              (10)
#define ESP_PANEL_LCD_RGB_TIMING_HFP              (50)
#define ESP_PANEL_LCD_RGB_TIMING_VPW              (2)
#define ESP_PANEL_LCD_RGB_TIMING_VBP              (18)
#define ESP_PANEL_LCD_RGB_TIMING_VFP              (8)
#define ESP_PANEL_LCD_RGB_PCLK_ACTIVE_NEG         (0)     // Waveshare demo: rising edge.
#define ESP_PANEL_LCD_RGB_DATA_WIDTH              (16)
#define ESP_PANEL_LCD_RGB_PIXEL_BITS              (16)    // 24 | 16
#define ESP_PANEL_LCD_RGB_FRAME_BUF_NUM           (2)     // Double buffer for tear-free IMU rotation.
// Direct PSRAM scan-out is not stable on ESP32-S3; use two internal-RAM
// bounce buffers. 20 lines keeps the ISR rate moderate without using too much SRAM.
#define ESP_PANEL_LCD_RGB_BOUNCE_BUF_SIZE         (10 * ESP_PANEL_LCD_HEIGHT)


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Please update the following configuration according to your board spec ////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Arduino-ESP32 4.x uses the strongly typed gpio_num_t enum here.
#define RGB_GPIO(n) static_cast<gpio_num_t>(n)
#define ESP_PANEL_LCD_PIN_NUM_RGB_HSYNC           RGB_GPIO(38)
#define ESP_PANEL_LCD_PIN_NUM_RGB_VSYNC           RGB_GPIO(39)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DE              RGB_GPIO(40)
#define ESP_PANEL_LCD_PIN_NUM_RGB_PCLK            RGB_GPIO(41)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DISP            RGB_GPIO(-1)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA0           RGB_GPIO(5)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA1           RGB_GPIO(45)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA2           RGB_GPIO(48)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA3           RGB_GPIO(47)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA4           RGB_GPIO(21)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA5           RGB_GPIO(14)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA6           RGB_GPIO(13)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA7           RGB_GPIO(12)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA8           RGB_GPIO(11)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA9           RGB_GPIO(10)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA10          RGB_GPIO(9)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA11          RGB_GPIO(46)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA12          RGB_GPIO(3)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA13          RGB_GPIO(8)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA14          RGB_GPIO(18)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA15          RGB_GPIO(17)

#define ESP_PANEL_LCD_BK_LIGHT_ON_LEVEL           (1)
#define ESP_PANEL_LCD_BK_LIGHT_OFF_LEVEL !ESP_PANEL_LCD_BK_LIGHT_ON_LEVEL


extern esp_lcd_panel_handle_t panel_handle;   
void ST7701_Init();

void LCD_Init();
void LCD_addWindow(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend,uint8_t* color);

// backlight
void Backlight_Init();
void Set_Backlight(uint8_t Light);    
