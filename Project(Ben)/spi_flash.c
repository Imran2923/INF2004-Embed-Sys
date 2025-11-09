// SPI0 pins by default: SCK=GP2, MOSI=GP3, MISO=GP4, CS=GP6.
// Change PIN_* and spi0->spi1 if you rewired.
//
// SD card uses FatFs (same style as your SD snippet). CSV stored at 0:/pico_test/results.csv

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/spi.h"
#include "hardware/timer.h"
#include "ff.h"             // FatFs
#include "flash.h"
#include "bench.h"
#include "csvlog.h"
#include "analyze.h"
#include "ui.h"
#include "net.h"
#include "http_server.h"
#include "config.h"

/* =================== MAIN =================== */

int main(void) {
    stdio_init_all();
    // Give USB time to enumerate
    for (int i = 0; i < 5000 && !stdio_usb_connected(); ++i) sleep_ms(1);
    sleep_ms(200);

    // Init SPI (flash)
    spi_init(spi0, SPI_FREQ_HZ);
    spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);

    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    cs_high();

     // 1) Bring up Wi-Fi (but do NOT block forever)
    wifi_init_default();
    wifi_connect_blocking(WIFI_SSID, WIFI_PSK, 10000); // ok if this fails

    // 2) Start HTTP server regardless. It'll be reachable once Wi-Fi has an IP.
    if (wifi_connect_blocking(WIFI_SSID, WIFI_PSK, 15000)) {
        printf("HTTP server starting on http://%s\n", wifi_get_ip_str());
        http_server_init(NULL);
    } else {
        printf("WiFi not connected; continuing without web UI.\n");
    }

    while (true) {
        print_menu();
        int c = get_choice_blocking();
        printf("%c\r\n", c);

        switch (c) {
            case '1':
                // Run benchmarks, log to serial
                run_benchmarks(false);
                break;

            case '2':
                // Simple connection + verify test
                action_test_connection();
                break;

            case '3': {
                FRESULT fr = csv_begin();
                if (fr != FR_OK) {
                    printf("CSV logging disabled.\r\n");
                    run_benchmarks_with_trials(N_TRIALS, false, true);    // save averages if possible
                } else {
                    (void)csv_mark_session_start();                       // marker for undo
                    run_benchmarks_with_trials(N_TRIALS, true,  true);    // per-run + averages
                    csv_end();
                }
                break;
            }

            case '4': {
                // Read results from SD card
                 print_csv();     // csvlog.c handles mount + unmount inside it
                 break;
            }

            case '5':
                // Run benchmark demo with 100 runs (summary only)
                run_benchmarks_100(false);
                break;

            case '6': {
                // Persistent erase: scan for the last marker and truncate there
                csv_erase_last_session();  // csvlog.c handles mount + unmount inside
                break;
            }

            case '7': {
                identify_chip_from_bench_12mhz();
                break;

            } 

             case '8':                                  // <— NEW
                action_show_network_status();
                break;

            case 'q':
            case 'Q':
                printf("Exiting menu. Reset board to reopen.\r\n");
                return 0;

            default:
                printf("Unknown choice. Try again.\r\n");
                break;
        }
        
    }

    // Shouldn’t reach here
    return 0;
}


