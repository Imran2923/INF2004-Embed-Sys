#include <stdio.h>
#include "pico/stdlib.h"
#include "ui.h"
#include "net.h"
#include "http_server.h"
#include "flash.h"
#include "ff.h"
#include "config.h"

int get_choice_blocking(void) {
    int c = PICO_ERROR_TIMEOUT;
    while (c == PICO_ERROR_TIMEOUT) {
        c = getchar_timeout_us(50 * 1000); // 50 ms poll
        tight_loop_contents();
    }
    return c;
}

void action_backup_flash(void) {
    printf("\r\n=== Backup SPI Flash ===\r\n");
    FRESULT fr = flash_backup_to_file("0:/pico_test/flash_backup.bin", FLASH_TOTAL_BYTES);
    if (fr == FR_OK) {
        printf("Backup OK -> 0:/pico_test/flash_backup.bin\r\n");
    } else {
        printf("ERROR: Backup failed (fr=%d). Check SD card and path.\r\n", fr);
    }
}

void action_restore_flash(void) {
    printf("\r\n=== Restore SPI Flash ===\r\n");

    // Safety checks: file exists, size matches capacity, JEDEC sanity
    FRESULT fr;
    FILINFO fno;
    fr = f_stat("0:/pico_test/flash_backup.bin", &fno);
    if (fr != FR_OK) {
        printf("ERROR: File not found: 0:/pico_test/flash_backup.bin (fr=%d)\r\n", fr);
        return;
    }
    if ((uint32_t)fno.fsize != FLASH_TOTAL_BYTES) {
        printf("ERROR: File size (%lu) != FLASH_TOTAL_BYTES (%u). Aborting restore.\r\n",
               (unsigned long)fno.fsize, (unsigned)FLASH_TOTAL_BYTES);
        return;
    }

    uint8_t id[3] = {0};
    read_jedec_id(id);
    printf("Current JEDEC: %02X %02X %02X\r\n", id[0], id[1], id[2]);
    // Optionally you can enforce a specific JEDEC here by comparing to your known chip.

    // Do the restore with verification enabled
    fr = flash_restore_from_file("0:/pico_test/flash_backup.bin", FLASH_TOTAL_BYTES, true);
    if (fr == FR_OK) {
        printf("Restore OK (verified).\r\n");
    } else {
        printf("ERROR: Restore failed (fr=%d). Content may be partial.\r\n", fr);
    }
}

void action_show_network_status(void) {
    const bool wifi_up = wifi_is_connected();
    const bool http_up = http_server_is_running();
    const char *ip = wifi_up ? wifi_get_ip_str() : "-";

    printf("\r\n=== Network Status ===\r\n");
    printf("WiFi: %s\r\n", wifi_up ? "Connected" : "Not connected");
    printf("IP:   %s\r\n", ip);
    printf("HTTP: %s", http_up ? "Running" : "Stopped");
    if (http_up) {
        printf(" (port %u)", http_server_port());
    }
    printf("\r\n======================\r\n");
}

void print_menu(void) {
    printf("\r\n\r\n=== SPI Flash Tool ===\r\n");
    printf("1: Run Benchmark (summary only)\r\n");
    printf("2: Run Test Connection\r\n");
    printf("3: Run Benchmark and Save Results (per-run + averages)\r\n");
    printf("4: Read Results (dump results.csv)\r\n");
    printf("5: Run Benchmark (100-run demo, summary only)\r\n");
    printf("6: Erase last saved test from results.csv\r\n");
    printf("7: Identify Chip (uses 12 MHz averages)\r\n");
    printf("8: Show server status\r\n");
    printf("b: Backup Flash chip data to SD\r\n");
    printf("r: Restore Flash chip data from SD\r\n"); 
    printf("q: Quit\r\n");
    printf("> ");
}
