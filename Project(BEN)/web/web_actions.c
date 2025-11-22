#include "web_actions.h"
#include "web_output.h"
#include "web_pages.h" 
#include "flash.h"
#include "csvlog.h"
#include "analyze.h"
#include "config.h"
#include "bench.h"
#include "net.h"
#include "http_server.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "ff.h"

void web_test_connection(void) {
    reset_web_output();
    
    web_printf("=== Test Connection ===\r\n\r\n");

    uint8_t id[3] = {0};
    read_jedec_id(id);

    web_printf("JEDEC ID: %02X %02X %02X\r\n", id[0], id[1], id[2]);

    uint8_t sr1 = read_status(0x05);
    uint8_t sr2 = read_status(0x35);

    web_printf("SR1: %02X  SR2: %02X\r\n", sr1, sr2);

    if (id[0] == 0x00 && id[1] == 0x00 && id[2] == 0x00) {
        web_printf("Result: FAILED - device not responding.\r\n");
    } else {
        web_printf("Result: PASSED - device responding and readable.\r\n");
    }

    web_printf("=== Done ===\r\n");
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

void web_run_benchmark(void) {
    reset_web_output();
    run_fast_benchmark_with_output(web_printf);  
}

void web_run_benchmark_save(void) {
    reset_web_output();
    web_printf("=== Running Benchmark + Save ===\r\n\r\n");
    web_printf("This will take 1-2 minutes. Saving to SD card...\r\n\r\n");
    
    FRESULT fr = csv_begin();
    if (fr != FR_OK) {
        web_printf("CSV logging disabled.\r\n");
        run_benchmarks_with_trials_web_safe(N_TRIALS, false, true, (printf_func_t)web_printf);
    } else {
        (void)csv_mark_session_start();
        run_benchmarks_with_trials_web_safe(N_TRIALS, true, true, (printf_func_t)web_printf);
        csv_end();
    }
    
    web_printf("\r\n=== Benchmark + Save Complete ===\r\n");
}

void web_run_benchmark_100(void) {
    reset_web_output();
    web_printf("=== Running 100-run Benchmark ===\r\n\r\n");
    web_printf("This will take approximately 2 minutes...\r\n\r\n");
    
    run_fast_benchmark_with_output(web_printf);  // Use the web-safe fast benchmark
    
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