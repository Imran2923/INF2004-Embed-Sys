#include "flash.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "config.h"
#include "ff.h"

void cs_low(void)  { gpio_put(PIN_CS, 0); }
void cs_high(void) { gpio_put(PIN_CS, 1); }

// Detect chip size by reading JEDEC and SFDP
uint32_t detect_chip_size(void) {
    uint8_t id[3];
    read_jedec_id(id);
    
    printf("JEDEC ID: %02X %02X %02X\n", id[0], id[1], id[2]);
    
    // Common chip sizes based on JEDEC ID
    // Manufacturer ID in id[0], Memory Type in id[1], Capacity in id[2]
    switch(id[1]) {
        case 0x40: 
            // Winbond memory type - check capacity byte
            switch(id[2]) {
                case 0x16: return 2 * 1024 * 1024;   // 2MB - W25Q16
                case 0x17: return 4 * 1024 * 1024;   // 4MB - W25Q32
                case 0x18: return 8 * 1024 * 1024;   // 8MB - W25Q64
                case 0x19: return 16 * 1024 * 1024;  // 16MB - W25Q128  
                case 0x20: return 32 * 1024 * 1024;  // 32MB - W25Q256
                case 0x21: return 64 * 1024 * 1024;  // 64MB - W25Q512
                default:   return 16 * 1024 * 1024;  // Default to 16MB
            }
        case 0x20: return 4 * 1024 * 1024;   // 4MB - MX25L32
        case 0x30: return 8 * 1024 * 1024;   // 8MB - GD25Q64
        case 0x60: return 16 * 1024 * 1024;  // 16MB - Other manufacturers
        default:   
            printf("Unknown memory type: %02X, defaulting to 16MB\n", id[1]);
            return 16 * 1024 * 1024;  // Default to 16MB
    }
}

// Backup entire chip to SD card
bool backup_entire_chip(const char* filename) {
    uint32_t chip_size = detect_chip_size();
    printf("Backing up %lu bytes to %s\n", chip_size, filename);
    
    FATFS fs;
    if(f_mount(&fs, "0:", 1) != FR_OK) return false;
    
    FIL file;
    if(f_open(&file, filename, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        f_mount(0, "", 0);
        return false;
    }
    
    uint8_t buffer[4096];
    uint32_t addr = 0;
    bool success = true;
    
    while(addr < chip_size && success) {
        uint32_t chunk_size = (chip_size - addr) > sizeof(buffer) ? sizeof(buffer) : (chip_size - addr);
        
        // Read from flash
        read_data(addr, buffer, chunk_size);
        
        // Write to file
        UINT bytes_written;
        if(f_write(&file, buffer, chunk_size, &bytes_written) != FR_OK || bytes_written != chunk_size) {
            success = false;
            break;
        }
        
        addr += chunk_size;
        
        // Progress indicator
        if(addr % (512 * 1024) == 0) {
            printf("Backup progress: %lu/%lu bytes\n", addr, chip_size);
        }
    }
    
    f_close(&file);
    f_mount(0, "", 0);
    
    if(success) {
        printf("Backup completed successfully: %s\n", filename);
    } else {
        printf("Backup failed at address 0x%06lX\n", addr);
    }
    
    return success;
}

// Restore entire chip from SD card
bool restore_entire_chip(const char* filename) {
    printf("Restoring from %s\n", filename);
    
    FATFS fs;
    if(f_mount(&fs, "0:", 1) != FR_OK) return false;
    
    FIL file;
    if(f_open(&file, filename, FA_READ) != FR_OK) {
        f_mount(0, "", 0);
        return false;
    }
    
    uint32_t file_size = f_size(&file);
    uint8_t buffer[256];  // Use smaller buffer for writing
    uint32_t addr = 0;
    bool success = true;
    
    // Erase the entire chip first (in 4K sectors)
    printf("Erasing chip...\n");
    for(uint32_t sector = 0; sector < file_size; sector += 4096) {
        sector_erase_4k(sector);
    }
    
    printf("Programming chip...\n");
    while(addr < file_size && success) {
        UINT bytes_read;
        if(f_read(&file, buffer, sizeof(buffer), &bytes_read) != FR_OK) {
            success = false;
            break;
        }
        
        if(bytes_read > 0) {
            // Program this chunk
            page_program(addr, buffer, bytes_read);
            addr += bytes_read;
            
            // Progress indicator
            if(addr % (512 * 1024) == 0) {
                printf("Restore progress: %lu/%lu bytes\n", addr, file_size);
            }
        } else {
            break; // EOF
        }
    }
    
    f_close(&file);
    f_mount(0, "", 0);
    
    if(success) {
        printf("Restore completed successfully\n");
    } else {
        printf("Restore failed at address 0x%06lX\n", addr);
    }
    
    return success;
}

void flash_init_spi(uint32_t hz){
    spi_init(spi0, hz);
    spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    cs_high();
}

void read_jedec_id(uint8_t id[3]){
    uint8_t tx[4] = {0x9F, 0, 0, 0}, rx[4] = {0};
    cs_low(); spi_write_read_blocking(spi0, tx, rx, 4); cs_high();
    id[0]=rx[1]; id[1]=rx[2]; id[2]=rx[3];
}

uint8_t read_status(uint8_t which){
    uint8_t tx[2] = {which, 0}, rx[2] = {0};
    cs_low(); spi_write_read_blocking(spi0, tx, rx, 2); cs_high();
    return rx[1];
}

bool read_sfdp_header(uint8_t hdr8[8]){
    uint8_t cmd[5] = {0x5A,0,0,0,0};
    cs_low(); spi_write_blocking(spi0, cmd, 5);
    spi_read_blocking(spi0, 0x00, hdr8, 8); cs_high();
    return hdr8[0]==0x53 && hdr8[1]==0x46 && hdr8[2]==0x44 && hdr8[3]==0x50;
}

void write_enable(void){
    uint8_t cmd=0x06; cs_low(); spi_write_blocking(spi0,&cmd,1); cs_high();
}

void wait_wip_clear(void){
    while (read_status(0x05) & 1) { sleep_ms(1); }
}

void read_data(uint32_t addr, uint8_t *buf, uint32_t len){
    uint8_t hdr[4] = {0x03, (uint8_t)(addr>>16),(uint8_t)(addr>>8),(uint8_t)addr};
    cs_low(); spi_write_blocking(spi0, hdr, 4);
    spi_read_blocking(spi0, 0x00, buf, (int)len); cs_high();
}

void page_program(uint32_t addr, const uint8_t *data, uint32_t len){
    write_enable();
    uint8_t hdr[4] = {0x02, (uint8_t)(addr>>16),(uint8_t)(addr>>8),(uint8_t)addr};
    cs_low(); spi_write_blocking(spi0, hdr, 4);
    spi_write_blocking(spi0, data, (int)len); cs_high();
    wait_wip_clear();
}

void sector_erase_4k(uint32_t addr){
    write_enable();
    uint8_t cmd[4] = {0x20,(uint8_t)(addr>>16),(uint8_t)(addr>>8),(uint8_t)addr};
    cs_low(); spi_write_blocking(spi0, cmd, 4); cs_high();
    wait_wip_clear();
}

// Many SPI NORs support JEDEC soft reset: 0x66 (Reset Enable), then 0x99 (Reset)
void flash_soft_reset(void){
    uint8_t cmd;
    cmd = 0x66; cs_low(); spi_write_blocking(spi0, &cmd, 1); cs_high();
    sleep_us(2); // tSHSL tiny gap
    cmd = 0x99; cs_low(); spi_write_blocking(spi0, &cmd, 1); cs_high();
    // give the device time to internally reset (datasheets: ~30–50us usually)
    sleep_ms(1);
}

// Some parts also wake from deep power-down with 0xAB (no harm if not in DPD)
void flash_release_from_dp(void){
    uint8_t cmd = 0xAB;
    cs_low(); spi_write_blocking(spi0, &cmd, 1); cs_high();
    sleep_us(50);
}

void flash_recover_to_safe_mode(void){
    // Try both: release-from-DP and soft-reset (harmless if not needed)
    flash_release_from_dp();
    flash_soft_reset();
    // Drop SPI speed to something very safe for JEDEC ID reads
    spi_init(spi0, SAFE_PROG_HZ);
    cs_high();
    sleep_ms(1);
}