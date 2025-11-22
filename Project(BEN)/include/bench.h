#pragma once
#include <stdint.h>
#include <stdbool.h>

// ========== Function Pointer Type ==========
typedef void (*printf_func_t)(const char *format, ...);

// ========== Original Benchmark Functions ==========

// Convenience wrappers (use your default trials, e.g., 10 and 100)
void run_benchmarks(bool save_per_run);
void run_benchmarks_100(bool save_per_run);

// One-shot smoke test: erase → program → read-back + print
void action_test_connection(void);

// ========== Dual-Core Fast Benchmark API ==========
// These functions allow running benchmarks on Core 1 while Core 0 handles WiFi/web

/**
 * @brief Initialize Core 1 for background benchmark execution
 * Call this once during startup or before first benchmark
 */
void init_benchmark_core(void);

/**
 * @brief Start a fast benchmark on Core 1 (non-blocking)
 * @return true if benchmark started, false if already running
 * 
 * This runs a reduced benchmark (2 frequencies × 2 trials = ~600ms)
 * on Core 1, leaving Core 0 free to handle web server requests.
 */
bool start_benchmark(void);

/**
 * @brief Check if a benchmark is currently running
 * @return true if benchmark is in progress
 */
bool is_benchmark_running(void);

/**
 * @brief Check if the last benchmark completed
 * @return true if benchmark finished (results available)
 */
bool is_benchmark_complete(void);

/**
 * @brief Get current benchmark progress
 * @return Progress percentage (0-100)
 */
int get_benchmark_progress(void);

/**
 * @brief Get current benchmark status message
 * @param out Buffer to write status string
 * @param len Size of output buffer
 * 
 * Status messages include things like "Testing @ 12000000 Hz..."
 */
void get_benchmark_status(char *out, size_t len);

/**
 * @brief Get benchmark results (when complete)
 * @param out Buffer to write results string
 * @param len Size of output buffer
 * 
 * Results include timing data, throughput, and any errors
 */
void get_benchmark_results(char *out, size_t len);

/**
 * @brief Reset benchmark state to idle (ready for next run)
 * 
 * Call this after retrieving results to prepare for next benchmark
 */
void reset_benchmark(void);

// ========== Web-Safe Benchmark Functions ==========
void run_fast_benchmark_with_output(printf_func_t output_func);
void run_benchmark_100_with_output(printf_func_t output_func);
void run_benchmarks_with_trials_web_safe(int trials, bool save_per_run, bool save_averages, printf_func_t output_func);
void run_fast_benchmark_web_safe(void);