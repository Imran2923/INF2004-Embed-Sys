#include "sd_card.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include <string.h>
#include <stdio.h>

// Helper function to select SD card
static inline void sd_cs_select(sd_card_t *sd) {
    gpio_put(sd->cs_pin, 0);
    sleep_us(10);
}

// Helper function to deselect SD card
static inline void sd_cs_deselect(sd_card_t *sd) {
    gpio_put(sd->cs_pin, 1);
    sleep_us(10);
}

// Send dummy bytes
static void sd_send_dummy_bytes(sd_card_t *sd, uint count) {
    uint8_t dummy = 0xFF;
    for (uint i = 0; i < count; i++) {
        spi_write_blocking(sd->spi, &dummy, 1);
    }
}

// Wait for card to be ready
static bool sd_wait_ready(sd_card_t *sd, uint32_t timeout_ms) {
    uint8_t response;
    absolute_time_t timeout = make_timeout_time_ms(timeout_ms);
    
    do {
        spi_read_blocking(sd->spi, 0xFF, &response, 1);
        if (response == 0xFF) {
            return true;
        }
    } while (!time_reached(timeout));
    
    return false;
}

// Send command to SD card
static uint8_t sd_send_command(sd_card_t *sd, uint8_t cmd, uint32_t arg) {
    uint8_t response;
    uint8_t buffer[6];
    
    // Wait for card to be ready
    if (!sd_wait_ready(sd, 500)) {
        return 0xFF;
    }
    
    // Prepare command packet
    buffer[0] = 0x40 | cmd;
    buffer[1] = (arg >> 24) & 0xFF;
    buffer[2] = (arg >> 16) & 0xFF;
    buffer[3] = (arg >> 8) & 0xFF;
    buffer[4] = arg & 0xFF;
    
    // CRC (only matters for CMD0 and CMD8)
    if (cmd == CMD0) {
        buffer[5] = 0x95;
    } else if (cmd == CMD8) {
        buffer[5] = 0x87;
    } else {
        buffer[5] = 0x01;
    }
    
    // Send command
    sd_cs_select(sd);
    spi_write_blocking(sd->spi, buffer, 6);
    
    // Wait for response (up to 10 attempts)
    for (int i = 0; i < 10; i++) {
        spi_read_blocking(sd->spi, 0xFF, &response, 1);
        if ((response & 0x80) == 0) {
            return response;
        }
    }
    
    return 0xFF;
}

// Send application-specific command
static uint8_t sd_send_app_command(sd_card_t *sd, uint8_t cmd, uint32_t arg) {
    sd_send_command(sd, CMD55, 0);
    return sd_send_command(sd, cmd, arg);
}

// Initialize SD card
bool sd_init(sd_card_t *sd) {
    uint8_t response;
    uint8_t ocr[4];
    
    // Setup CS pin
    gpio_init(sd->cs_pin);
    gpio_set_dir(sd->cs_pin, GPIO_OUT);
    gpio_put(sd->cs_pin, 1);
    
    // Setup SPI - SLOWER for initialization
    printf("SD Card: Setting up SPI1 at 100kHz\n");
    printf("SD Card: MISO=%d, SCK=%d, MOSI=%d, CS=%d\n", 
           SD_PIN_MISO, SD_PIN_SCK, SD_PIN_MOSI, sd->cs_pin);

    spi_init(sd->spi, 100 * 1000); // Changed to 100kHz
    gpio_set_function(SD_PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_MOSI, GPIO_FUNC_SPI);
    
    // Send at least 74 clock cycles with CS high
    sd_cs_deselect(sd);
    sd_send_dummy_bytes(sd, 10);

    // NOW select the card and send more clocks
    printf("SD CARD: Powering up card with CS low...\n");
    sd_cs_select(sd);
    sd_send_dummy_bytes(sd, 10);
    sd_cs_deselect(sd);
    
    printf("SD Card: Waiting 200ms for card to wake up...\n");
    sleep_ms(500);  // Wait longer for SD card to be ready
    
    // Reset card (CMD0) - try multiple times
    printf("SD Card: Sending CMD0 (attempt 1)...\n");
    response = sd_send_command(sd, CMD0, 0);
    sd_cs_deselect(sd);
    
    if (response != R1_IDLE_STATE) {
        printf("SD Card: CMD0 attempt 1 failed (0x%02X), retrying...\n", response);
        sleep_ms(100);
        
        printf("SD Card: Sending CMD0 (attempt 2)...\n");
        response = sd_send_command(sd, CMD0, 0);
        sd_cs_deselect(sd);
    }
    
    if (response != R1_IDLE_STATE) {
        printf("SD Card: CMD0 attempt 2 failed (0x%02X), retrying...\n", response);
        sleep_ms(100);
        
        printf("SD Card: Sending CMD0 (attempt 3)...\n");
        response = sd_send_command(sd, CMD0, 0);
        sd_cs_deselect(sd);
    }
    
    if (response != R1_IDLE_STATE) {
        printf("SD Card: CMD0 failed after 3 attempts (0x%02X)\n", response);
        return false;
    }
    
    printf("SD Card: CMD0 success\n");
    
    // Check voltage range (CMD8)
    response = sd_send_command(sd, CMD8, 0x1AA);
    
    if (response == R1_IDLE_STATE) {
        // SD Card v2
        printf("SD Card: Version 2.0 detected\n");
        
        // Read OCR
        spi_read_blocking(sd->spi, 0xFF, ocr, 4);
        sd_cs_deselect(sd);
        
        if ((ocr[2] & 0x01) && (ocr[3] == 0xAA)) {
            // Initialize card (ACMD41)
            uint32_t timeout = 1000; // 1 second timeout
            absolute_time_t end_time = make_timeout_time_ms(timeout);
            
            do {
                response = sd_send_app_command(sd, ACMD41, 0x40000000);
                sd_cs_deselect(sd);
                
                if (response == 0) {
                    break;
                }
                
                sleep_ms(10);
            } while (!time_reached(end_time));
            
            if (response != 0) {
                printf("SD Card: ACMD41 timeout\n");
                return false;
            }
            
            // Check CCS bit in OCR
            response = sd_send_command(sd, CMD58, 0);
            if (response == 0) {
                spi_read_blocking(sd->spi, 0xFF, ocr, 4);
                sd_cs_deselect(sd);
                
                if (ocr[0] & 0x40) {
                    sd->type = SD_CARD_TYPE_SDHC;
                    printf("SD Card: SDHC/SDXC\n");
                } else {
                    sd->type = SD_CARD_TYPE_SD2;
                    printf("SD Card: Standard Capacity v2\n");
                }
            }
        }
    } else {
        // SD Card v1 or MMC
        sd_cs_deselect(sd);
        printf("SD Card: Version 1.0 detected\n");
        sd->type = SD_CARD_TYPE_SD1;
        
        // Initialize card (ACMD41)
        uint32_t timeout = 1000;
        absolute_time_t end_time = make_timeout_time_ms(timeout);
        
        do {
            response = sd_send_app_command(sd, ACMD41, 0);
            sd_cs_deselect(sd);
            
            if (response == 0) {
                break;
            }
            
            sleep_ms(10);
        } while (!time_reached(end_time));
        
        if (response != 0) {
            printf("SD Card: ACMD41 timeout\n");
            return false;
        }
    }
    
    // Set block size to 512 bytes (CMD16)
    if (sd->type != SD_CARD_TYPE_SDHC) {
        response = sd_send_command(sd, CMD16, SD_BLOCK_SIZE);
        sd_cs_deselect(sd);
        
        if (response != 0) {
            printf("SD Card: CMD16 failed\n");
            return false;
        }
    }
    
    // Speed up SPI to 12.5 MHz
    spi_set_baudrate(sd->spi, 12500 * 1000);
    
    printf("SD Card: Initialization complete\n");
    sd->initialized = true;
    
    return true;
}

// Read single block from SD card
bool sd_read_block(sd_card_t *sd, uint32_t block, uint8_t *buffer) {
    uint8_t response;
    uint8_t token;
    uint32_t address;
    
    if (!sd->initialized) {
        return false;
    }
    
    // Convert block number to byte address for non-SDHC cards
    if (sd->type == SD_CARD_TYPE_SDHC) {
        address = block;
    } else {
        address = block * SD_BLOCK_SIZE;
    }
    
    // Send read command
    response = sd_send_command(sd, CMD17, address);
    
    if (response != 0) {
        sd_cs_deselect(sd);
        printf("SD Card: CMD17 failed (0x%02X)\n", response);
        return false;
    }
    
    // Wait for data token
    absolute_time_t timeout = make_timeout_time_ms(500);
    do {
        spi_read_blocking(sd->spi, 0xFF, &token, 1);
        if (token == TOKEN_START_BLOCK) {
            break;
        }
    } while (!time_reached(timeout));
    
    if (token != TOKEN_START_BLOCK) {
        sd_cs_deselect(sd);
        printf("SD Card: Data token timeout\n");
        return false;
    }
    
    // Read data
    spi_read_blocking(sd->spi, 0xFF, buffer, SD_BLOCK_SIZE);
    
    // Read CRC (2 bytes) and discard
    uint8_t crc[2];
    spi_read_blocking(sd->spi, 0xFF, crc, 2);
    
    sd_cs_deselect(sd);
    
    return true;
}

// Write single block to SD card
bool sd_write_block(sd_card_t *sd, uint32_t block, const uint8_t *buffer) {
    uint8_t response;
    uint32_t address;
    
    if (!sd->initialized) {
        return false;
    }
    
    // Convert block number to byte address for non-SDHC cards
    if (sd->type == SD_CARD_TYPE_SDHC) {
        address = block;
    } else {
        address = block * SD_BLOCK_SIZE;
    }
    
    // Send write command
    response = sd_send_command(sd, CMD24, address);
    
    if (response != 0) {
        sd_cs_deselect(sd);
        printf("SD Card: CMD24 failed (0x%02X)\n", response);
        return false;
    }
    
    // Send data token
    uint8_t token = TOKEN_START_BLOCK;
    spi_write_blocking(sd->spi, &token, 1);
    
    // Send data
    spi_write_blocking(sd->spi, buffer, SD_BLOCK_SIZE);
    
    // Send dummy CRC
    uint8_t crc[2] = {0xFF, 0xFF};
    spi_write_blocking(sd->spi, crc, 2);
    
    // Read data response
    spi_read_blocking(sd->spi, 0xFF, &response, 1);
    
    if ((response & 0x1F) != 0x05) {
        sd_cs_deselect(sd);
        printf("SD Card: Write failed (0x%02X)\n", response);
        return false;
    }
    
    // Wait for write to complete
    if (!sd_wait_ready(sd, 500)) {
        sd_cs_deselect(sd);
        printf("SD Card: Write timeout\n");
        return false;
    }
    
    sd_cs_deselect(sd);
    
    return true;
}

// Get number of sectors
uint32_t sd_get_num_sectors(sd_card_t *sd) {
    // This is a simplified version - in a full implementation,
    // you would read the CSD register to get the actual capacity
    return sd->sectors;
}