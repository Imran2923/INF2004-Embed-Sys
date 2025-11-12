#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "ff.h"   // FatFs

// File locations
#define CSV_PATH   "0:/pico_test/results.csv"
#define BENCH_PATH "0:/pico_test/benchmark.csv"

// Per-run CSV (results.csv)
FRESULT csv_begin(void);
void    csv_end(void);
void    csv_row_to_sd(bool save, int run, const char* op, uint32_t hz,
                      uint32_t addr, uint32_t bytes, int64_t dur_us,
                      double mbps, uint32_t verify_errors, uint8_t sr1_end);

// Session markers (to erase the latest saved test)
DWORD   csv_mark_session_start(void);
FRESULT csv_erase_last_session(void);

// Utility to print the current results.csv to serial (optional)
FRESULT print_csv(void);

// Averages CSV (benchmark.csv)
FRESULT bench_csv_begin(void);
void bench_csv_append_avg(const char *jedec_hex,
                          uint32_t hz,
                          double avg_erase_ms,
                          double avg_write_kBps,
                          double avg_readseq_kBps,
                          double avg_readrand_MBps,
                          uint32_t verify_errors);
void    bench_csv_end(void);
FRESULT csv_truncate_to(DWORD pos);
void csv_undo_current_session(void);