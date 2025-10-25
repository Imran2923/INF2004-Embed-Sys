// hw_config.c — Maker Pi Pico(W) microSD on SPI1
#include "pico/stdlib.h"
#include "sd_driver/spi.h"
#include "sd_driver/sd_card.h"
#include "sd_driver/hw_config.h"

#define COUNT_OF(x) (sizeof(x) / sizeof(x[0]))

// SPI1: SCK=GP10, MOSI/CMD=GP11, MISO/DAT0=GP12
static spi_t spis[] = {
    {
        .hw_inst = spi1,
        .miso_gpio = 12,
        .mosi_gpio = 11,
        .sck_gpio  = 10,
        .baud_rate = 1000000, // 1 MHz for init; library can bump later
        .set_drive_strength = true,
        .mosi_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
        .sck_gpio_drive_strength  = GPIO_DRIVE_STRENGTH_4MA,
    }
};

// SD socket with CS on GP15
static sd_card_t sd_cards[] = {
    {
        .pcName = "0:",           // logical drive name for FatFs
        .spi = &spis[0],
        .ss_gpio = 15,            // CS/DAT3 = GP15
        .use_card_detect = false, // socket has no CD switch; assume present
        .card_detect_gpio = 0,
        .card_detected_true = 0,
        .set_drive_strength = true,
        .ss_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
    }
};

// Required by the library
size_t   spi_get_num(void)             { return COUNT_OF(spis); }
spi_t   *spi_get_by_num(size_t num)    { return (num < COUNT_OF(spis)) ? &spis[num] : NULL; }
size_t   sd_get_num(void)              { return COUNT_OF(sd_cards); }
sd_card_t*sd_get_by_num(size_t num)    { return (num < COUNT_OF(sd_cards)) ? &sd_cards[num] : NULL; }
