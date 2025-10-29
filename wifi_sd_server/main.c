#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/gpio.h"  // ADD THIS LINE!
#include "sd_card.h"
#include "http_server.h"

// WiFi Configuration - CHANGE THESE!
#define WIFI_SSID "Ben10"
#define WIFI_PASSWORD "xzrn7855"

// LED blink pattern
void blink_led(int times, int delay_ms) {
    for (int i = 0; i < times; i++) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        sleep_ms(delay_ms);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        sleep_ms(delay_ms);
    }
}

int main() {
    // Initialize stdio
    stdio_init_all();
    
    // Wait a moment for USB serial to connect
    sleep_ms(2000);
    
    printf("\n");
    printf("==================================================\n");
    printf("🚀 Pico W SD Card Web Server Starting...\n");
    printf("==================================================\n\n");
    
    // Initialize WiFi
    printf("Initializing WiFi...\n");
    if (cyw43_arch_init()) {
        printf("❌ Failed to initialize WiFi\n");
        return 1;
    }
    
    // Blink LED to show startup
    blink_led(3, 200);
    
    // Enable station mode
    cyw43_arch_enable_sta_mode();
    
    printf("Connecting to WiFi '%s'...\n", WIFI_SSID);
    
    // Connect to WiFi
    int wifi_status = cyw43_arch_wifi_connect_timeout_ms(
        WIFI_SSID, 
        WIFI_PASSWORD, 
        CYW43_AUTH_WPA2_AES_PSK, 
        30000
    );
    
    if (wifi_status != 0) {
        printf("❌ Failed to connect to WiFi (error %d)\n", wifi_status);
        printf("\nPlease check:\n");
        printf("- SSID is correct: '%s'\n", WIFI_SSID);
        printf("- Password is correct\n");
        printf("- Network is 2.4GHz (Pico W doesn't support 5GHz)\n");
        printf("- Network security is WPA2\n");
        return 1;
    }
    
    printf("✅ WiFi connected!\n");
    
    // Get IP address
    extern cyw43_t cyw43_state;
    uint32_t ip_addr = cyw43_state.netif[CYW43_ITF_STA].ip_addr.addr;
    printf("📡 IP Address: %lu.%lu.%lu.%lu\n",
           ip_addr & 0xFF,
           (ip_addr >> 8) & 0xFF,
           (ip_addr >> 16) & 0xFF,
           (ip_addr >> 24) & 0xFF);
    
    // ====== ADD SD CARD PIN TEST HERE ======
    printf("\n=== Testing SD Card Pins ===\n");
    printf("Pin Configuration:\n");
    printf("  SPI Port: spi1\n");
    printf("  MISO (GP%d)\n", SD_PIN_MISO);
    printf("  MOSI (GP%d)\n", SD_PIN_MOSI);
    printf("  SCK  (GP%d)\n", SD_PIN_SCK);
    printf("  CS   (GP%d)\n", SD_PIN_CS);
    
    printf("\nTesting CS pin toggle...\n");
    gpio_init(SD_PIN_CS);
    gpio_set_dir(SD_PIN_CS, GPIO_OUT);
    
    // Toggle CS pin 5 times
    for (int i = 0; i < 5; i++) {
        gpio_put(SD_PIN_CS, 1);
        printf("  CS = HIGH\n");
        sleep_ms(100);
        gpio_put(SD_PIN_CS, 0);
        printf("  CS = LOW\n");
        sleep_ms(100);
    }
    gpio_put(SD_PIN_CS, 1); // Leave high
    printf("CS pin test complete\n");
    printf("===========================\n\n");
    // ====== END PIN TEST ======
    
    // Initialize SD card
    printf("Initializing SD card...\n");
    sd_card_t sd_card = {
        .spi = SD_SPI_PORT,
        .cs_pin = SD_PIN_CS,
        .type = SD_CARD_TYPE_UNKNOWN,
        .sectors = 0,
        .initialized = false
    };
    
    if (!sd_init(&sd_card)) {
        printf("❌ Failed to initialize SD card\n");
        printf("\nPlease check:\n");
        printf("- SD card is inserted\n");
        printf("- SD card is formatted as FAT32\n");
        printf("- SD card is not write-protected\n");
        printf("- SD card size is 2GB-32GB\n");
        printf("\nContinuing without SD card...\n");
    } else {
        printf("✅ SD card initialized successfully\n");
    }
    
    // Initialize HTTP server
    printf("\nStarting HTTP server...\n");
    http_server_init(&sd_card);
    
    printf("\n");
    printf("==================================================\n");
    printf("✅ Server is running!\n");
    printf("📡 Access from your browser: http://%lu.%lu.%lu.%lu\n",
           ip_addr & 0xFF,
           (ip_addr >> 8) & 0xFF,
           (ip_addr >> 16) & 0xFF,
           (ip_addr >> 24) & 0xFF);
    printf("==================================================\n\n");
    
    // Turn on LED to indicate server is running
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    
    // Main loop
    while (true) {
        // The HTTP server runs in background via lwIP callbacks
        // Just keep the program alive
        sleep_ms(1000);
        
        // Optional: Toggle LED to show alive status
        static bool led_state = true;
        led_state = !led_state;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_state);
    }
    
    // Cleanup (never reached in this example)
    cyw43_arch_deinit();
    
    return 0;
}