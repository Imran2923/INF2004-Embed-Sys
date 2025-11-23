#include "flash.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "ff.h"          // FatFs
#include <string.h>
#include "config.h"  // where PIN_* live

#ifndef FLASH_PAGE_SIZE
#define FLASH_PAGE_SIZE   256u       // common for SPI NOR
#endif
#ifndef FLASH_ERASE_SIZE
#define FLASH_ERASE_SIZE  4096u      // 4KB sectors
#endif

static FATFS g_fs;

void cs_low(void)  { gpio_put(PIN_CS, 0); }
void cs_high(void) { gpio_put(PIN_CS, 1); }

static FRESULT ensure_sd_and_folder(void) {
    FRESULT fr = f_mount(&g_fs, "0:", 1);
    if (fr != FR_OK) { printf("f_mount error: %d\r\n", fr); return fr; }
    f_mkdir("0:/pico_test"); // ok if exists
    return FR_OK;
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
    cmd = 0x66; 
    cs_low(); 
    spi_write_blocking(spi0, &cmd, 1); 
    cs_high();
    sleep_us(2);
    cmd = 0x99; 
    cs_low(); 
    spi_write_blocking(spi0, &cmd, 1); 
    cs_high();
    sleep_ms(1);
}

void wait_wip_clear_web_safe(void){
    while (read_status(0x05) & 1) { 
        tight_loop_contents();  // ← Web-safe!
    }
}

void flash_release_from_dp(void){
    uint8_t cmd = 0xAB;
    cs_low(); 
    spi_write_blocking(spi0, &cmd, 1); 
    cs_high();
    sleep_us(50);
}

void flash_recover_to_safe_mode(void){
    flash_release_from_dp();
    flash_soft_reset();
    spi_init(spi0, 12000000);  // Safe 12MHz
    cs_high();
    sleep_ms(1);
}

// ========== BACKUP: Dump entire flash to SD card ==========
FRESULT flash_backup_to_file(const char *path, uint32_t flash_bytes) {
    FRESULT fr;
    FIL f;
    UINT bw = 0;
    uint8_t buf[512];
    
    printf("DEBUG: Mounting SD card...\r\n");
    fr = ensure_sd_and_folder();
    if (fr != FR_OK) {
        printf("ERROR: SD mount failed (error %d)\r\n", fr);
        return fr;
    }
    printf("DEBUG: SD mounted OK\r\n");

    printf("DEBUG: Creating file: %s\r\n", path);
    fr = f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) { 
        printf("ERROR: Failed to create file (error %d)\r\n", fr);
        f_unmount("0:");
        return fr;
    }
    printf("DEBUG: File opened for writing\r\n");

    printf("Backing up %u bytes...\r\n", (unsigned)flash_bytes);

    uint32_t total_written = 0;
    
    for (uint32_t addr = 0; addr < flash_bytes; addr += sizeof(buf)) {
        uint32_t chunk = (flash_bytes - addr > sizeof(buf)) ? sizeof(buf) : (flash_bytes - addr);
        
        // Read from flash
        read_data(addr, buf, chunk);
        
        // Write to SD
        bw = 0;
        fr = f_write(&f, buf, (UINT)chunk, &bw);
        
        if (fr != FR_OK) {
            printf("\r\nERROR: Write failed at 0x%06X (error %d)\r\n", addr, fr);
            goto cleanup;
        }
        
        if (bw != chunk) {
            printf("\r\nERROR: Partial write at 0x%06X (wrote %u/%u)\r\n", 
                   addr, (unsigned)bw, (unsigned)chunk);
            fr = FR_DISK_ERR;
            goto cleanup;
        }
        
        total_written += bw;
        
        // Sync every 64KB to force actual SD writes
        if ((addr % (64 * 1024)) == 0) {
            fr = f_sync(&f);
            if (fr != FR_OK) {
                printf("\r\nERROR: Sync failed at 0x%06X (error %d)\r\n", addr, fr);
                goto cleanup;
            }
        }
        
        // Progress every 256KB
        if ((addr & ((256u * 1024u) - 1)) == 0) {
            printf(".");
            fflush(stdout);
        }
    }
    
    printf("\r\nWrote %u bytes total\r\n", (unsigned)total_written);

cleanup:
    printf("DEBUG: Final sync...\r\n");
    fr = f_sync(&f);
    if (fr != FR_OK) {
        printf("ERROR: Final sync failed (error %d)\r\n", fr);
    }
    
    printf("DEBUG: Closing file...\r\n");
    FRESULT close_result = f_close(&f);
    if (close_result != FR_OK) {
        printf("ERROR: Close failed (error %d)\r\n", close_result);
        fr = close_result;
    }
    
    printf("DEBUG: Unmounting...\r\n");
    f_unmount("0:");
    
    if (fr == FR_OK) {
        printf("SUCCESS: Backup complete\r\n");
    } else {
        printf("FAILED: Error code %d\r\n", fr);
    }
    
    return fr;
}

// ========== RESTORE (with web-safe option) ==========
FRESULT flash_restore_from_file(const char *path, uint32_t flash_bytes, bool verify) {
    FRESULT fr;
    FIL f;
    FILINFO finfo;
    UINT br;
    static uint8_t buf[FLASH_ERASE_SIZE];
    static uint8_t rb [FLASH_ERASE_SIZE];

    printf("DEBUG: Mounting SD card...\r\n");
    fr = ensure_sd_and_folder();
    if (fr != FR_OK) {
        printf("ERROR: SD mount failed (error %d)\r\n", fr);
        return fr;
    }

    fr = f_stat(path, &finfo);
    if (fr != FR_OK) {
        printf("ERROR: File not found: %s (error %d)\r\n", path, fr);
        f_unmount("0:");
        return fr;
    }
    printf("Found: %s (size: %lu bytes)\r\n", path, (unsigned long)finfo.fsize);

    fr = f_open(&f, path, FA_READ | FA_OPEN_EXISTING);
    if (fr != FR_OK) {
        printf("ERROR: Failed to open (error %d)\r\n", fr);
        f_unmount("0:");
        return fr;
    }

    uint32_t todo = (uint32_t)f_size(&f);
    if (todo == 0) {
        printf("ERROR: File size is 0\r\n");
        f_close(&f);
        f_unmount("0:");
        return FR_INT_ERR;
    }
    if (todo > flash_bytes) {
        printf("WARNING: File is %u bytes, limiting to %u\r\n", 
               (unsigned)todo, (unsigned)flash_bytes);
        todo = flash_bytes;
    }

    printf("Restoring %u bytes%s...\r\n", 
           (unsigned)todo, verify ? " with verify" : "");

    for (uint32_t base = 0; base < todo; base += FLASH_ERASE_SIZE) {
        uint32_t want = FLASH_ERASE_SIZE;
        if (base + want > todo) want = todo - base;

        br = 0;
        fr = f_read(&f, buf, (UINT)want, &br);
        if (fr != FR_OK) {
            printf("\r\nERROR: Read failed at 0x%06X (error %d)\r\n", base, fr);
            break;
        }
        if (br == 0) {
            printf("\r\nERROR: Unexpected EOF at 0x%06X\r\n", base);
            fr = FR_INT_ERR;
            break;
        }

        // ERASE with web-safe wait
        write_enable();
        uint8_t cmd[4] = {0x20,(uint8_t)(base>>16),(uint8_t)(base>>8),(uint8_t)base};
        cs_low(); 
        spi_write_blocking(spi0, cmd, 4); 
        cs_high();
        wait_wip_clear_web_safe();  // Web-safe!

        // PROGRAM in pages with web-safe wait
        for (uint32_t off = 0; off < br; off += FLASH_PAGE_SIZE) {
            uint32_t page_size = (br - off > FLASH_PAGE_SIZE) ? FLASH_PAGE_SIZE : (br - off);
            
            write_enable();
            uint8_t hdr[4] = {0x02, (uint8_t)((base+off)>>16),(uint8_t)((base+off)>>8),(uint8_t)(base+off)};
            cs_low(); 
            spi_write_blocking(spi0, hdr, 4);
            spi_write_blocking(spi0, &buf[off], page_size); 
            cs_high();
            wait_wip_clear_web_safe();  // Web-safe!
        }

        // VERIFY
        if (verify) {
            read_data(base, rb, br);
            if (memcmp(buf, rb, br) != 0) {
                printf("\r\nERROR: VERIFY FAILED at 0x%06X\r\n", base);
                fr = FR_INT_ERR;
                break;
            }
        }

        if ((base & ((256u * 1024u) - 1u)) == 0) {
            printf(".");
            fflush(stdout);
        }
    }

    printf("\r\n");
    if (fr == FR_OK) {
        printf("SUCCESS: Restore complete%s\r\n", verify ? " (verified)" : "");
        f_sync(&f);
    } else {
        printf("FAILED: Error code %d\r\n", fr);
    }

    f_close(&f);
    f_unmount("0:");
    return fr;
}