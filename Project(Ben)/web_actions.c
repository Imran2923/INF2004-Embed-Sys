#include "web_actions.h"
#include "web_output.h"
#include "web_pages.h" 
#include "flash.h"
#include "csvlog.h"
#include "analyze.h"
#include "config.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "ff.h"


void web_backup_chip(void) {
    reset_web_output();
    web_printf("=== Chip Backup ===\r\n\r\n");
    
    // Check if backup already exists
    FATFS fs;
    if(f_mount(&fs, "0:", 1) == FR_OK) {
        FILINFO fno;
        if(f_stat("0:/pico_test/chip_backup.bin", &fno) == FR_OK) {
            web_printf("Backup file already exists!\r\n");
            web_printf("Size: %lu bytes\r\n", fno.fsize);
            web_printf("Delete existing backup or use restore function.\r\n");
            f_mount(0, "", 0);
            return;
        }
        f_mount(0, "", 0);
    }
    
    // Proceed with backup
    if(backup_entire_chip("0:/pico_test/chip_backup.bin")) {
        web_printf("Backup completed successfully\r\n");
        web_printf("Saved to: 0:/pico_test/chip_backup.bin\r\n");
    } else {
        web_printf("Backup failed!\r\n");
    }
}

void web_restore_chip(void) {
    reset_web_output();
    web_printf("=== Chip Restore ===\r\n\r\n");
    
    // Check if backup file exists
    FATFS fs;
    if(f_mount(&fs, "0:", 1) == FR_OK) {
        FILINFO fno;
        if(f_stat("0:/pico_test/chip_backup.bin", &fno) != FR_OK) {
            web_printf("Backup file not found!\r\n");
            web_printf("Please run backup first.\r\n");
            f_mount(0, "", 0);
            return;
        }
        f_mount(0, "", 0);
    }
    
    // Proceed with restore
    if(restore_entire_chip("0:/pico_test/chip_backup.bin")) {
        web_printf("Restore completed successfully\r\n");
    } else {
        web_printf("Restore failed!\r\n");
    }
}

void web_test_connection(void) {
    reset_web_output();
    
    web_printf("=== Test Connection ===\r\n\r\n");

    uint8_t id[3] = {0};
    read_jedec_id(id);
    web_printf("JEDEC ID: %02X %02X %02X\r\n", id[0], id[1], id[2]);
    web_printf("SR1: %02X  SR2: %02X\r\n", read_status(0x05), read_status(0x35));

    spi_init(spi0, SAFE_PROG_HZ);

    const uint32_t base_addr = SCRATCH_BASE;
    const uint32_t page_addr = base_addr & ~0xFFu;
    const uint8_t  msg[] = "Hello, Flash!\r\n";

    web_printf("Erasing 4K @0x%06X...\r\n", base_addr);
    
    // Use the same timing approach as your existing bench.c
    uint32_t start_time = time_us_32();  // Use time_us_32() instead
    sector_erase_4k(base_addr);
    uint32_t end_time = time_us_32();
    uint8_t sr1 = read_status(0x05);
    web_printf("Erase took %lu us, SR1=%02X\r\n", (end_time - start_time), sr1);

    uint8_t page[256];
    for (int i=0;i<256;i++) page[i]=0xFF;
    for (size_t i=0;i<sizeof(msg);i++) page[i]=msg[i];

    web_printf("Programming 256 bytes...\r\n");
    write_enable();
    uint8_t hdr[4] = {0x02, (uint8_t)(page_addr>>16),(uint8_t)(page_addr>>8),(uint8_t)page_addr};
    cs_low(); 
    spi_write_blocking(spi0, hdr, 4);
    spi_write_blocking(spi0, page, 256); 
    cs_high();
    wait_wip_clear();
    sr1 = read_status(0x05);

    uint8_t rb[256];
    read_data(page_addr, rb, 256);
    uint32_t errors = 0;
    for (int i = 0; i < 256; i++) if (rb[i] != page[i]) errors++;
    
    web_printf("Verification %s (errors=%u). SR1=%02X\r\n",
               errors ? "FAILED" : "PASSED", errors, sr1);

    web_printf("Read-back (32B): ");
    for (int i=0;i<32;i++) web_printf("%02X ", rb[i]);
    web_printf("\r\n=== Done ===\r\n");
}

void web_read_results(void) {
    reset_web_output();
    web_printf("=== Results CSV ===\r\n\r\n");
    
    FATFS fs;
    FRESULT fr = f_mount(&fs, "0:", 1);
    if (fr != FR_OK) {
        web_printf("ERROR: SD mount failed (%d)\r\n", fr);
        return;
    }

    FIL f;
    fr = f_open(&f, CSV_PATH, FA_READ);
    if (fr != FR_OK) {
        web_printf("ERROR: Could not open %s (%d)\r\n", CSV_PATH, fr);
        f_mount(0, "", 0);
        return;
    }

    char line[256];
    while (f_gets(line, sizeof line, &f)) {
        web_printf("%s", line);
    }
    
    f_close(&f);
    f_mount(0, "", 0);
    web_printf("\r\n=== End of File ===\r\n");
}

void web_erase_last_session(void) {
    reset_web_output();
    web_printf("Erasing last session...\r\n");
    csv_erase_last_session();
    web_printf("Last session erased from results.csv\r\n");
}

void web_identify_chip(void) {
    reset_web_output();
    web_printf("Identifying chip...\r\n");
    identify_chip_from_bench_12mhz();
    web_printf("NOTE: Chip identification output appears on serial monitor only for now.\r\n");
    web_printf("Check serial port for identification results.\r\n");
}
// Add these to web_actions.c
void web_run_benchmark(void) {
    reset_web_output();
    web_printf("=== Running Benchmark ===\r\n\r\n");
    run_benchmarks(false);  // Calls your existing bench function
    web_printf("\r\n=== Benchmark Complete ===\r\n");
}

void web_run_benchmark_save(void) {
    reset_web_output();
    web_printf("=== Running Benchmark + Save ===\r\n\r\n");
    FRESULT fr = csv_begin();
    if (fr != FR_OK) {
        web_printf("CSV logging disabled.\r\n");
        run_benchmarks_with_trials(N_TRIALS, false, true);
    } else {
        (void)csv_mark_session_start();
        run_benchmarks_with_trials(N_TRIALS, true, true);
        csv_end();
    }
    web_printf("\r\n=== Benchmark + Save Complete ===\r\n");
}

void web_run_benchmark_100(void) {
    reset_web_output();
    web_printf("=== Running 100-run Demo ===\r\n\r\n");
    run_benchmarks_100(false);  // Your existing function
    web_printf("\r\n=== 100-run Demo Complete ===\r\n");
}
void web_show_status(void) {
    reset_web_output();
    web_printf("=== System Status ===\r\n\r\n");
    web_printf("WiFi: %s\r\n", wifi_is_connected() ? "Connected" : "Disconnected");
    web_printf("IP: %s\r\n", wifi_get_ip_str());
    web_printf("HTTP Server: %s\r\n", http_server_is_running() ? "Running" : "Stopped");
    web_printf("SD Card: %s\r\n", sd_ok() ? "Connected" : "Not Connected");
}