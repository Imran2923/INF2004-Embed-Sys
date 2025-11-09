#include <stdio.h>
#include "pico/stdlib.h"
#include "ui.h"
#include "net.h"
#include "http_server.h"

int get_choice_blocking(void) {
    int c = PICO_ERROR_TIMEOUT;
    while (c == PICO_ERROR_TIMEOUT) {
        c = getchar_timeout_us(50 * 1000); // 50 ms poll
        tight_loop_contents();
    }
    return c;
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
    printf("q: Quit\r\n");
    printf("> ");
}
