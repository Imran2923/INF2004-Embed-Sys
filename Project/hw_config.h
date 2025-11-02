#pragma once
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include <stdbool.h>
#include <stddef.h>

// include the driver's type so prototypes match exactly
#include "no-OS-FatFS-SD-SPI-RPi-Pico/FatFs_SPI/sd_driver/sd_card.h"

// Return pointer to the N-th SD card (0-based). Driver calls this.
sd_card_t *sd_get_by_num(size_t num);


// On-board microSD uses SPI1 on GP10..GP12 with CS on GP15
#define SD_SPI_PORT         spi1
#define SD_PIN_SPI_SCK      10
#define SD_PIN_SPI_MOSI     11
#define SD_PIN_SPI_MISO     12
#define SD_PIN_SPI_CS       15

// These boards typically have no dedicated Card-Detect or Write-Protect
#define SD_PIN_CARD_DETECT  -1
#define SD_PIN_WRITE_PROTECT -1

// Start slow for init; raise after card is alive
#define SD_SPI_BAUD_INIT_HZ  400000
#define SD_SPI_BAUD_RUN_HZ  12000000

// Prototypes used by the sd_card shim from pico-examples
void sd_card_gpio_init(void);
void sd_card_spi_init(void);
bool sd_card_detect(sd_card_t *sd);
