#pragma once
#include <stdint.h>
#include <stdbool.h>

// Run the full benchmark:
//  - trials: how many runs per SPI frequency
//  - save_per_run: if true, per-measurement rows go to results.csv (call csv_begin() in main before this)
//  - save_averages: if true, one average row per SPI freq goes to benchmark.csv (bench.c handles open/close)
void run_benchmarks_with_trials(int trials, bool save_per_run, bool save_averages);

// Convenience wrappers (use your default trials, e.g., 10 and 100)
void run_benchmarks(bool save_per_run);
void run_benchmarks_100(bool save_per_run);

// One-shot smoke test: erase → program → read-back + print
void action_test_connection(void);
