#pragma once
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
// =======================
// Global build-time config
// =======================

// ---- SPI Flash (spi0) pins ----
#define PIN_SCK   2
#define PIN_MOSI  3
#define PIN_MISO  4
#define PIN_CS    6

// ---- SPI frequencies (Hz) you want to sweep for benchmarks ----
#define N_FREQS   3

// Conservative clock just for program+verify (helps with breadboard wiring)
#define SAFE_PROG_HZ  12000000u

// ---- Benchmark parameters ----
#define N_TRIALS  100          // default trials for "Run Benchmark"
#define READ_SEQ_SIZE      (256u * 1024u)  // 256 KB sequential read window
#define RAND_READ_ITERS    16u         // number of 256B random reads per run

// Scratch region where we’re allowed to erase/program (avoid reserved areas)
#define SCRATCH_BASE   0x000000u
#define SCRATCH_SIZE   (256u * 1024u)  // must be >= 4KB and multiple of 4KB

// ---- Operation timeouts (microseconds) ----
#define TOUT_ERASE_US  (800 * 1000)    // 4KB erase timeout (adjust per chip)
#define TOUT_PROG_US   (5 * 1000)      // 256B program timeout

// Raise after wiring is proven solid (try 8 or 12 MHz)
#define SPI_FREQ_HZ       (4 * 1000 * 1000)   // 4 MHz

/* ========== "Fast benchmark" sizes (quick per-run work) ========== */
#define RUNS                100

#define ERASE_ADDR_BASE     0x000000u     // we step 4KB per run
#define WRITE_ADDR_BASE     0x000000u     // 4KB per run, 100 runs => 400KB total
#define WRITE_BYTES_PER_RUN (4 * 1024u)   // 4 KB
#define READ_ADDR_BASE      0x000000u
#define READ_BYTES_PER_RUN  (64 * 1024u)  // 64 KB

/* =================== SD CARD / CSV CONFIG =================== */
#define SD_MOUNT_POINT   "0:"
#define SD_DIR           "0:/pico_test"
#define CSV_PATH         "0:/pico_test/results.csv"

// === Benchmark Averages CSV (summary) ===
#define BENCH_PATH "0:/pico_test/benchmark.csv"

/* =================== SPI FLASH HELPERS =================== */

#define CSV_PATH "0:/pico_test/results.csv"

// 64Mbit ISSI IS25LP064A = 8 * 1024 * 1024 bytes
#ifndef FLASH_TOTAL_BYTES
#define FLASH_TOTAL_BYTES (8u * 1024u * 1024u)
#endif

#define WIFI_SSID "TakoFi"
#define WIFI_PSK  "Enderdragon16"
