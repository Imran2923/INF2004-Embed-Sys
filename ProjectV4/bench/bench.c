#include <stdio.h>
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/spi.h"
#include "pico/cyw43_arch.h"

#include "bench.h"
#include "flash.h"
#include "csvlog.h"
#include "config.h"

// If you have a central config.h, include it. Otherwise these fallbacks keep it building.
#ifdef __has_include
#  if __has_include("config.h")
#    include "config.h"
#  endif
#endif

#ifndef N_TRIALS
#  define N_TRIALS 10
#endif

#ifndef READ_SEQ_SIZE
#  define READ_SEQ_SIZE (64u * 1024u)   // 256 KB sequential read window
#endif

#ifndef RAND_READ_ITERS
#  define RAND_READ_ITERS 8u
#endif

#ifndef SAFE_PROG_HZ
#  define SAFE_PROG_HZ 12000000u         // safer clock for program+verify if wiring is marginal
#endif

#ifndef N_FREQS
#  define N_FREQS 3
#endif

#ifndef SPI_FREQS
static const uint32_t SPI_FREQS[N_FREQS] = { 12000000u, 24000000u, 36000000u };
#endif

#ifndef SCRATCH_BASE
#  define SCRATCH_BASE (FLASH_SIZE_BYTES - 0x10000)
#endif

#ifndef SCRATCH_SIZE
#  define SCRATCH_SIZE (256u * 1024u)    // choose an area you know is safe to clobber
#endif

#ifndef TOUT_ERASE_US
#  define TOUT_ERASE_US (800*1000)       // conservative erase timeout (adjust per chip)
#endif

#ifndef TOUT_PROG_US
#  define TOUT_PROG_US (5*1000)          // conservative program timeout
#endif

// Choose conservative timeouts (ms). You can tune these per chip later.
#define ERASE_WEB_TIMEOUT_MS  5000u   // up to 5s for slow parts
#define PROG_WEB_TIMEOUT_MS    100u   // 100ms is plenty for 256

uint32_t max_safe_read_hz;
uint32_t max_safe_write_hz;
uint32_t erase_timeout_ms;


// Web-safe WIP wait: no sleep_ms, just polling + Wi-Fi poll.
// Used only by the *_web_safe() functions.
// Web-safe WIP wait: no sleep_ms, but with a real timeout.
static bool wait_wip_clear_web_safe(uint32_t timeout_ms, printf_func_t out) {
    if (!out) out = printf;   // allow NULL -> printf

    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);

    for (;;) {
        uint8_t sr1 = read_status(0x05);
        if ((sr1 & 0x01u) == 0) {
            // WIP cleared
            return true;
        }

        // Check timeout
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
            out("WARN: WIP timeout in wait_wip_clear_web_safe (SR1=%02X)\r\n", sr1);
            return false;
        }

        // Let Wi-Fi/LWIP breathe
        cyw43_arch_poll();
        tight_loop_contents();
    }
}



// Erase a 4KB sector, web-safe, with timeout
static bool sector_erase_4k_web_safe(uint32_t addr, uint32_t timeout_ms, printf_func_t out) {
    uint8_t buf[4];
    buf[0] = 0x20; // 4K sector erase
    buf[1] = (addr >> 16) & 0xFF;
    buf[2] = (addr >> 8)  & 0xFF;
    buf[3] = (addr >> 0)  & 0xFF;

    write_enable();
    cs_low();
    spi_write_blocking(spi0, buf, 4);
    cs_high();

    return wait_wip_clear_web_safe(timeout_ms, out);
}

// Program 256 bytes, web-safe, with timeout
static bool page_program_web_safe(uint32_t addr, const uint8_t *data,
                                  uint32_t timeout_ms, printf_func_t out) {
    uint8_t cmd = 0x02; // PAGE PROGRAM (256B)

    write_enable();
    cs_low();
    spi_write_blocking(spi0, &cmd, 1);

    uint8_t addr_buf[3];
    addr_buf[0] = (addr >> 16) & 0xFF;
    addr_buf[1] = (addr >> 8)  & 0xFF;
    addr_buf[2] = (addr >> 0)  & 0xFF;
    spi_write_blocking(spi0, addr_buf, 3);
    spi_write_blocking(spi0, data, 256);
    cs_high();

    return wait_wip_clear_web_safe(timeout_ms, out);
}

// ------------------ small helpers ------------------

static inline double _mbps(uint32_t bytes, int64_t us) {
    if (us <= 0) return 0.0;
    return (double)bytes / (1024.0 * 1024.0) / ((double)us / 1e6);
}

static inline uint32_t _xorshift32(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x; return x;
}

static inline uint32_t _rand_addr_in_scratch(uint32_t *seed) {
    uint32_t off = _xorshift32(seed) % (SCRATCH_SIZE - 256u);
    off &= ~0xFFu; // page alignment
    return SCRATCH_BASE + off;
}

// ------------------ timed primitives (use flash.c API) ------------------

static int64_t timed_erase_4k(uint32_t addr, uint8_t *sr1_end) {
    absolute_time_t t0 = get_absolute_time();
    sector_erase_4k(addr);        // flash.c: includes WREN + WIP wait
    int64_t us = absolute_time_diff_us(t0, get_absolute_time());
    if (sr1_end) *sr1_end = read_status(0x05);
    return us;
}

int64_t timed_erase_4k_web(uint32_t addr, uint8_t *out_sr1) {
    absolute_time_t t0 = get_absolute_time();

    bool ok = sector_erase_4k_web_safe(addr, ERASE_WEB_TIMEOUT_MS, NULL);
    uint8_t sr1 = read_status(0x05);
    if (out_sr1) *out_sr1 = sr1;

    if (!ok) {
        // signal failure with negative value
        return -1;
    }

    int64_t us = absolute_time_diff_us(t0, get_absolute_time());
    return us;
}

// Programs exactly 256 bytes at page-aligned addr; reads back and counts mismatches.
static int64_t timed_prog_256(uint32_t addr, const uint8_t *page,
                              uint32_t *verify_errs, uint8_t *sr1_end)
{
    addr &= ~0xFFu;

    // Program
    absolute_time_t t0 = get_absolute_time();
    page_program(addr, page, 256);      // flash.c: does WREN + WIP wait
    int64_t us = absolute_time_diff_us(t0, get_absolute_time());
    if (sr1_end) *sr1_end = read_status(0x05);

    // Verify
    uint8_t rb[256];
    read_data(addr, rb, 256);
    uint32_t e = 0;
    for (int i = 0; i < 256; i++) if (rb[i] != page[i]) e++;
    if (verify_errs) *verify_errs = e;
    return us;
}

// Web-safe version of timed_prog_256
int64_t timed_prog_256_web(uint32_t addr, const uint8_t *data,
                           uint32_t *verify_errors, uint8_t *out_sr1) {
    absolute_time_t t0 = get_absolute_time();

    bool ok = page_program_web_safe(addr, data, PROG_WEB_TIMEOUT_MS, NULL);
    uint8_t sr1 = read_status(0x05);
    if (out_sr1) *out_sr1 = sr1;

    if (!ok) {
        if (verify_errors) *verify_errors = 1; // mark as failed
        return -1;
    }

    // Optional verify (like your normal timed_prog_256)
    uint8_t rb[256];
    read_data(addr, rb, sizeof rb);

    uint32_t verr = 0;
    for (int i = 0; i < 256; ++i) {
        if (rb[i] != data[i]) ++verr;
    }
    if (verify_errors) *verify_errors = verr;

    int64_t us = absolute_time_diff_us(t0, get_absolute_time());
    return us;
}

// Efficient sequential read timing without allocating a huge buffer.
static int64_t timed_read_seq(uint32_t addr, uint32_t len) {
    uint8_t buf[256];
    uint32_t left = len;
    uint32_t cur  = addr;

    absolute_time_t t0 = get_absolute_time();
    while (left) {
        uint32_t chunk = left > sizeof(buf) ? sizeof(buf) : left;
        read_data(cur, buf, chunk);
        cur  += chunk;
        left -= chunk;
    }
    return absolute_time_diff_us(t0, get_absolute_time());
}

static int64_t timed_read_rand256(uint32_t *seed, uint32_t *out_addr) {
    uint32_t addr = _rand_addr_in_scratch(seed);
    if (out_addr) *out_addr = addr;
    uint8_t buf[256];
    absolute_time_t t0 = get_absolute_time();
    read_data(addr, buf, 256);
    return absolute_time_diff_us(t0, get_absolute_time());
}

// ------------------ public actions ------------------

void action_test_connection(void) {
    printf("\r\n=== Test Connection (Non-Destructive) ===\r\n");

    // Always use a safe clock when reading JEDEC
    spi_init(spi0, SAFE_PROG_HZ);
    cs_high();

    uint8_t id[3] = {0};
    read_jedec_id(id);
    printf("JEDEC ID: %02X %02X %02X\r\n", id[0], id[1], id[2]);

    uint8_t sr1 = read_status(0x05);
    uint8_t sr2 = read_status(0x35);

    printf("SR1: %02X  SR2: %02X\r\n", sr1, sr2);

    // pass/fail purely based on JEDEC readability
    if (id[0] == 0x00 && id[1] == 0x00 && id[2] == 0x00) {
        printf("Result: FAILED - device not responding.\r\n");
    } else {
        printf("Result: PASSED - device responding and readable.\r\n");
    }

    printf("=== Done ===\r\n");
}

// Main benchmark runner
void run_benchmarks_with_trials(int trials, bool save_per_run, bool save_averages) {
    // Open averages CSV if requested
    if (save_averages) {
        FRESULT fr = bench_csv_begin();
        if (fr != FR_OK) {
            printf("WARNING: benchmark.csv not opened; averages will not be saved.\r\n");
            save_averages = false;
        }
    }

    // Deterministic test pattern for programming
    uint8_t page[256];
    for (int i = 0; i < 256; i++) page[i] = (uint8_t)i;

    spi_init(spi0, SAFE_PROG_HZ);
    cs_high();

    // Optional: chip capability header
    uint8_t id[3] = {0};
    read_jedec_id(id);
    char jedec_hex[7];
    snprintf(jedec_hex, sizeof jedec_hex, "%02X%02X%02X", id[0], id[1], id[2]);
    uint8_t sfdp8[8] = {0};
    bool has_sfdp = read_sfdp_header(sfdp8);
    printf("# JEDEC=%02X %02X %02X  SFDP=%s\r\n",
           id[0], id[1], id[2], has_sfdp ? "OK" : "N/A");

    for (size_t fi = 0; fi < N_FREQS; ++fi) {
        uint32_t hz = SPI_FREQS[fi];
        spi_init(spi0, hz);

        // --- accumulators for averages ---
        double   sum_erase_us      = 0.0;
        double   sum_prog_mbps     = 0.0;
        double   sum_readseq_mbps  = 0.0;
        double   sum_readrand_mbps = 0.0;
        uint32_t total_verify_errs = 0;

        // --- new: min/max latency tracking (µs) ---
        double min_erase_us      = 1e30, max_erase_us      = 0.0;
        double min_prog_us       = 1e30, max_prog_us       = 0.0;
        double min_readseq_us    = 1e30, max_readseq_us    = 0.0;
        double min_readrand_us   = 1e30, max_readrand_us   = 0.0;

        for (int run = 1; run <= trials; ++run) {
            // rotate across sectors within scratch
            uint32_t sector_idx = (run - 1) % (SCRATCH_SIZE / 4096u);
            uint32_t era_addr   = SCRATCH_BASE + sector_idx * 4096u;
            uint32_t page_addr  = era_addr; // first page in that sector

            // -------- ERASE 4KB at SAFE_PROG_HZ --------
            spi_init(spi0, SAFE_PROG_HZ);
            cs_high();

            uint8_t sr1 = 0;
            int64_t us_erase = timed_erase_4k(era_addr, &sr1);
            sum_erase_us += (double)us_erase;

            if (us_erase < min_erase_us) min_erase_us = (double)us_erase;
            if (us_erase > max_erase_us) max_erase_us = (double)us_erase;

            if (save_per_run) {
                csv_row_to_sd(true, run, "ERASE_4K",
                              SAFE_PROG_HZ, era_addr, 4096u,
                              us_erase, 0.0, 0, sr1);
            }

            // -------- PROGRAM 256B at SAFE_PROG_HZ --------
            cs_high();

            uint32_t verr = 0;
            sr1 = 0;
            int64_t us_prog = timed_prog_256(page_addr, page, &verr, &sr1);
            total_verify_errs += verr;

            if (us_prog < min_prog_us) min_prog_us = (double)us_prog;
            if (us_prog > max_prog_us) max_prog_us = (double)us_prog;

            double prog_mbps = _mbps(256, us_prog);
            sum_prog_mbps += prog_mbps;

            if (save_per_run) {
                csv_row_to_sd(true, run, "PROG_256B",
                              SAFE_PROG_HZ, page_addr, 256u,
                              us_prog, prog_mbps, verr, sr1);
            }

            // -------- switch back to benchmark frequency for reads --------
            spi_init(spi0, hz);
            cs_high();

            // -------- READ SEQ over READ_SEQ_SIZE --------
            int64_t us_rseq = timed_read_seq(SCRATCH_BASE, READ_SEQ_SIZE);
            if (us_rseq < min_readseq_us) min_readseq_us = (double)us_rseq;
            if (us_rseq > max_readseq_us) max_readseq_us = (double)us_rseq;

            double rseq_mbps = _mbps(READ_SEQ_SIZE, us_rseq);
            sum_readseq_mbps += rseq_mbps;

            if (save_per_run) {
                csv_row_to_sd(true, run, "READ_SEQ",
                              hz, SCRATCH_BASE, READ_SEQ_SIZE,
                              us_rseq, rseq_mbps, 0, read_status(0x05));
            }

            // -------- READ RAND (many 256B samples) --------
            uint32_t seed = 0xC001D00Du ^ (uint32_t)run ^ (uint32_t)hz;
            double   rand_mbps_acc = 0.0;
            for (uint32_t i = 0; i < RAND_READ_ITERS; ++i) {
                uint32_t ra = 0;
                int64_t  us_rr = timed_read_rand256(&seed, &ra);

                if (us_rr < min_readrand_us) min_readrand_us = (double)us_rr;
                if (us_rr > max_readrand_us) max_readrand_us = (double)us_rr;

                double r_mb = _mbps(256, us_rr);
                rand_mbps_acc += r_mb;

                if (save_per_run) {
                    csv_row_to_sd(true, run, "READ_RAND",
                                  hz, ra, 256u,
                                  us_rr, r_mb, 0, read_status(0x05));
                }
            }
            sum_readrand_mbps += (rand_mbps_acc / (double)RAND_READ_ITERS);

            // Give the system a tiny breather every few runs to avoid lockups
            if ((run & 7) == 0) {
                sleep_ms(2);
            }
        }

        // --- derived averages ---
        double avg_erase_ms      = (sum_erase_us / trials) / 1000.0;
        double avg_prog_mbps     =  sum_prog_mbps      / trials;
        double avg_readseq_mbps  =  sum_readseq_mbps   / trials;
        double avg_readrand_mbps =  sum_readrand_mbps  / trials;

        // --- convert min/max into nice units ---
        double min_erase_ms      = min_erase_us      / 1000.0;
        double max_erase_ms      = max_erase_us      / 1000.0;
        double min_prog_us_out   = min_prog_us;
        double max_prog_us_out   = max_prog_us;
        double min_rseq_ms       = min_readseq_us    / 1000.0;
        double max_rseq_ms       = max_readseq_us    / 1000.0;
        double min_rrand_us_out  = min_readrand_us;
        double max_rrand_us_out  = max_readrand_us;

        printf("\r\n=== Benchmark (avg over %d runs) ===\r\n", trials);
        printf("SPI clock: %u Hz\r\n\r\n", hz);

        // Throughput + latency ranges
        printf("--- Erase 4KB ---\r\n");
        printf("Avg time:  %.2f ms\r\n", avg_erase_ms);
        printf("Latency range: min %.2f ms, max %.2f ms\r\n",
               min_erase_ms, max_erase_ms);

        printf("\r\n--- Write 256B ---\r\n");
        printf("Avg speed: %.2f KB/s (%.3f MB/s)\r\n",
               avg_prog_mbps * 1024.0, avg_prog_mbps);
        printf("Latency range: min %.2f µs, max %.2f µs\r\n",
               min_prog_us_out, max_prog_us_out);

        printf("\r\n--- Read %uKB (sequential) ---\r\n",
               (unsigned)(READ_SEQ_SIZE / 1024));
        printf("Avg speed: %.2f KB/s (%.3f MB/s)\r\n",
               avg_readseq_mbps * 1024.0, avg_readseq_mbps);
        printf("Latency range for %uKB block: min %.2f ms, max %.2f ms\r\n",
               (unsigned)(READ_SEQ_SIZE / 1024),
               min_rseq_ms, max_rseq_ms);

        printf("\r\n--- Read 256B (random) ---\r\n");
        printf("Avg speed: %.2f KB/s (%.3f MB/s)\r\n",
               avg_readrand_mbps * 1024.0, avg_readrand_mbps);
        printf("Per-transaction latency range: min %.2f µs, max %.2f µs\r\n",
               min_rrand_us_out, max_rrand_us_out);

        if (total_verify_errs) {
            printf("ERROR: Verify failed — %u mismatched byte(s) across %d run(s).\r\n",
                   total_verify_errs, trials);
            printf("Explanation: data read back did not match what was written.\r\n");
            printf("Common causes:\r\n");
            printf("  • Sector not erased before programming (must be 0xFF)\r\n");
            printf("  • SPI clock too high for write/verify on this wiring\r\n");
            printf("  • Page program crossing a 256-byte boundary\r\n");
            printf("  • Loose wiring / noisy signals (MISO/MOSI/SCK/CS)\r\n");
        }

        // Save averages (one row per SPI freq) to benchmark.csv if requested
        if (save_averages) {
            bench_csv_append_avg(jedec_hex,
                                 hz,
                                 avg_erase_ms,
                                 avg_prog_mbps    * 1024.0,
                                 avg_readseq_mbps * 1024.0,
                                 avg_readrand_mbps,
                                 total_verify_errs);
        }
    }

    if (save_averages) {
        bench_csv_end();
        printf("Saved averages to %s\r\n", BENCH_PATH);
    }
}

// ========== FAST BENCHMARK (WEB-SAFE) ==========
// 100-run web-safe benchmark
void run_benchmark_100_with_output(printf_func_t output_func) {
    output_func("=== 100-Run Benchmark (web-safe) ===\r\n\r\n");

    spi_init(spi0, SAFE_PROG_HZ);
    cs_high();
    
    // Read chip ID
    uint8_t id[3] = {0};
    read_jedec_id(id);
    output_func("JEDEC: %02X %02X %02X\r\n\r\n", id[0], id[1], id[2]);
    
    // Test pattern
    uint8_t page[256];
    for (int i = 0; i < 256; i++) page[i] = (uint8_t)i;
    
    const uint32_t TEST_FREQS[] = {12000000u, 24000000u, 36000000u};
    const int TRIALS = 100;
    
    for (size_t freq_idx = 0; freq_idx < 3; ++freq_idx) {
        uint32_t hz = TEST_FREQS[freq_idx];
        
        output_func("@ %u Hz:\r\n", hz);
        
        double   sum_erase_us = 0.0;
        double   sum_prog_us  = 0.0;
        double   sum_read_us  = 0.0;
        uint32_t total_errors = 0;
        
        for (int trial = 0; trial < TRIALS; ++trial) {
            // Progress indicator every 10 trials
            if (trial % 10 == 0) {
                output_func("  Progress: %d/%d trials...\r\n", trial, TRIALS);
            }
            
            uint32_t sector_addr = SCRATCH_BASE + (trial % 64) * 4096;

            // -------- ERASE (web-safe, SAFE_PROG_HZ) --------
            spi_init(spi0, SAFE_PROG_HZ);
            cs_high();

            uint8_t sr1 = 0;
            int64_t us  = timed_erase_4k_web(sector_addr, &sr1);
            sum_erase_us += (double)us;

            // -------- PROGRAM (web-safe, SAFE_PROG_HZ) --------
            uint32_t verr = 0;
            sr1 = 0;
            us  = timed_prog_256_web(sector_addr, page, &verr, &sr1);
            sum_prog_us += (double)us;
            total_errors += verr;

            // -------- READ (bench frequency, read-only) --------
            spi_init(spi0, hz);
            cs_high();

            absolute_time_t t0 = get_absolute_time();
            uint8_t buf[256];
            for (uint32_t off = 0; off < 2048; off += 256) {
                read_data(sector_addr + off, buf, 256);
            }
            us = absolute_time_diff_us(t0, get_absolute_time());
            sum_read_us += (double)us;

            // Breather every 10 trials to avoid starving USB/WiFi
            if ((trial % 10) == 9) {
                sleep_ms(5);
            }
        }
        
        // Calculate averages
        double avg_erase_ms   = (sum_erase_us / TRIALS) / 1000.0;
        double avg_prog_kbps  = (256.0  * TRIALS) / (sum_prog_us / 1e6) / 1024.0;
        double avg_read_kbps  = (2048.0 * TRIALS) / (sum_read_us / 1e6) / 1024.0;
        
        output_func("\r\n=== Results (avg over %d runs) ===\r\n", TRIALS);
        output_func("Erase 4KB: %.2f ms\r\n", avg_erase_ms);
        output_func("Write 256B: %.2f KB/s (%.3f MB/s)\r\n",
                    avg_prog_kbps,  avg_prog_kbps / 1024.0);
        output_func("Read 2KB:   %.2f KB/s (%.3f MB/s)\r\n\r\n",
                    avg_read_kbps,  avg_read_kbps / 1024.0);
        
        if (total_errors > 0) {
            output_func("WARNING: %u verify errors!\r\n\r\n", total_errors);
        }

        // Give a little pause between frequencies
        sleep_ms(20);
    }
    
    output_func("=== Complete ===\r\n");
}


void run_fast_benchmark_with_output(printf_func_t output_func) {
    output_func("=== Fast Benchmark (web-safe) ===\r\n\r\n");

    spi_init(spi0, SAFE_PROG_HZ);
    cs_high();
    
    uint8_t id[3] = {0};
    read_jedec_id(id);
    output_func("JEDEC: %02X %02X %02X\r\n\r\n", id[0], id[1], id[2]);
    
    uint8_t page[256];
    for (int i = 0; i < 256; i++) page[i] = (uint8_t)i;
    
    const uint32_t TEST_FREQS[] = {12000000u, 24000000u};
    const int TRIALS = 2;
    
    uint32_t total_errors = 0;
    
    for (size_t freq_idx = 0; freq_idx < 2; ++freq_idx) {
        uint32_t hz = TEST_FREQS[freq_idx];
        
        output_func("@ %u Hz:\r\n", hz);
        
        double sum_erase_us = 0.0;
        double sum_prog_us  = 0.0;
        double sum_read_us  = 0.0;
        
        for (int trial = 0; trial < TRIALS; ++trial) {
            uint32_t sector_addr = SCRATCH_BASE
                                 + (freq_idx * TRIALS + trial) * 4096;
            
            // ERASE at SAFE_PROG_HZ
            spi_init(spi0, SAFE_PROG_HZ);
            cs_high();
            uint8_t  sr1 = 0;
            int64_t  us  = timed_erase_4k_web(sector_addr, &sr1);
            sum_erase_us += (double)us;
            
            // PROGRAM at SAFE_PROG_HZ
            uint32_t verr = 0;
            sr1 = 0;
            us  = timed_prog_256_web(sector_addr, page, &verr, &sr1);
            sum_prog_us += (double)us;
            total_errors += verr;
            
            // READ at test frequency
            spi_init(spi0, hz);
            cs_high();
            absolute_time_t t0 = get_absolute_time();
            uint8_t buf[256];
            for (uint32_t off = 0; off < 4096; off += 256) {
                read_data(sector_addr + off, buf, 256);
            }
            us = absolute_time_diff_us(t0, get_absolute_time());
            sum_read_us += (double)us;
        }
        
        double avg_erase_ms   = (sum_erase_us / TRIALS) / 1000.0;
        double avg_prog_kbps  = (256.0  * TRIALS) / (sum_prog_us / 1e6) / 1024.0;
        double avg_read_kbps  = (4096.0 * TRIALS) / (sum_read_us / 1e6) / 1024.0;
        
        output_func("  Erase:  %.2f ms\r\n", avg_erase_ms);
        output_func("  Write:  %.2f KB/s (%.3f MB/s)\r\n",
                    avg_prog_kbps, avg_prog_kbps / 1024.0);
        output_func("  Read:   %.2f KB/s (%.3f MB/s)\r\n\r\n",
                    avg_read_kbps, avg_read_kbps / 1024.0);
    }
    
    if (total_errors > 0) {
        output_func("WARNING: %u verify errors!\r\n", total_errors);
    }
    
    output_func("=== Complete ===\r\n");
}

void run_benchmarks_with_trials_web_safe(int trials,
                                         bool save_per_run,
                                         bool save_averages,
                                         printf_func_t out)
{
    if (!out) {
        // No default to printf here to avoid type mismatch; just bail if NULL.
        return;
    }

    // Open averages CSV if requested
    if (save_averages) {
        FRESULT fr = bench_csv_begin();
        if (fr != FR_OK) {
            out("WARNING: benchmark.csv not opened; averages will not be saved.\r\n");
            save_averages = false;
        }
    }

    // Deterministic test pattern for programming
    uint8_t page[256];
    for (int i = 0; i < 256; i++) {
        page[i] = (uint8_t)i;
    }

    // --- Identify chip from JEDEC ---
    uint8_t id[3] = {0};
    read_jedec_id(id);
    uint32_t jedec = ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | (uint32_t)id[2];

    uint32_t max_safe_read_hz  = 12000000u;   // conservative defaults
    uint32_t max_safe_write_hz = 8000000u;
    uint32_t erase_timeout_ms  = 4000u;

    if (jedec == 0xEF4016) {
        // Winbond W25Q32FV
        out("Detected: Winbond W25Q32FV (JEDEC %02X %02X %02X)\r\n",
            id[0], id[1], id[2]);
        max_safe_read_hz  = 50000000u;   // rated up to 104 MHz, we test up to 50 MHz
        max_safe_write_hz = 12000000u;   // 12 MHz write is fine
        erase_timeout_ms  = 2000u;       // sector erase
    } else if (jedec == 0xBF2641) {
        // SST / Microchip 26F016B
        out("Detected: SST / Microchip 26F016B (JEDEC %02X %02X %02X)\r\n",
            id[0], id[1], id[2]);
        max_safe_read_hz  = 20000000u;   // keep reads at or below 20 MHz
        max_safe_write_hz = 8000000u;    // 8 MHz write to be safe
        erase_timeout_ms  = 4000u;       // give them more time
    } else {
        out("Unknown JEDEC: %02X %02X %02X, using conservative limits.\r\n",
            id[0], id[1], id[2]);
    }

    char jedec_hex[7];
    snprintf(jedec_hex, sizeof jedec_hex, "%02X%02X%02X", id[0], id[1], id[2]);

    uint8_t sfdp8[8] = {0};
    bool has_sfdp = read_sfdp_header(sfdp8);
    out("# JEDEC=%02X %02X %02X  SFDP=%s\r\n",
        id[0], id[1], id[2], has_sfdp ? "OK" : "N/A");

    // ------------------------------
    // Main loop over SPI frequencies
    // ------------------------------
    for (size_t fi = 0; fi < N_FREQS; ++fi) {
        uint32_t hz = SPI_FREQS[fi];

        // Skip unsafe read clocks for this chip
        if (hz > max_safe_read_hz) {
            out("\r\n[SKIP] %u Hz is above safe read clock (%u Hz) for this chip.\r\n",
                hz, max_safe_read_hz);
            continue;
        }

        out("\r\n=== Benchmark at %u Hz (avg over %d runs) ===\r\n",
            hz, trials);

        spi_init(spi0, hz);
        cs_high();

        double   sum_erase_us      = 0.0;
        double   sum_prog_mbps     = 0.0;
        double   sum_readseq_mbps  = 0.0;
        double   sum_readrand_mbps = 0.0;
        uint32_t total_verify_errs = 0;

        // latency ranges
        double min_erase_us      = 1e30, max_erase_us      = 0.0;
        double min_prog_us       = 1e30, max_prog_us       = 0.0;
        double min_readseq_us    = 1e30, max_readseq_us    = 0.0;
        double min_readrand_us   = 1e30, max_readrand_us   = 0.0;

        for (int run = 1; run <= trials; ++run) {
            if ((run % 10) == 0) {
                out("  Progress: %d/%d...\r\n", run, trials);
            }

            // Rotate across sectors within scratch
            uint32_t sector_idx = (uint32_t)(run - 1) % (SCRATCH_SIZE / 4096u);
            uint32_t era_addr   = SCRATCH_BASE + sector_idx * 4096u;
            uint32_t page_addr  = era_addr;

            // -------- ERASE 4KB (web-safe) --------
            uint8_t sr1 = 0;
            int64_t us_erase = timed_erase_4k_web(era_addr, &sr1);

            sum_erase_us += (double)us_erase;
            if (us_erase < min_erase_us) min_erase_us = (double)us_erase;
            if (us_erase > max_erase_us) max_erase_us = (double)us_erase;

            if (save_per_run) {
                csv_row_to_sd(true, run, "ERASE_4K",
                              hz, era_addr, 4096u,
                              us_erase, 0.0, 0, sr1);
            }

            // -------- PROGRAM 256B at safe write Hz --------
            uint32_t prog_hz = SAFE_PROG_HZ;
            if (prog_hz > max_safe_write_hz) {
                prog_hz = max_safe_write_hz;
            }
            spi_init(spi0, prog_hz);
            cs_high();

            uint32_t verr = 0;
            sr1 = 0;
            int64_t us_prog = timed_prog_256_web(page_addr, page, &verr, &sr1);
            total_verify_errs += verr;

            if (us_prog < min_prog_us) min_prog_us = (double)us_prog;
            if (us_prog > max_prog_us) max_prog_us = (double)us_prog;

            double prog_mbps = _mbps(256, us_prog);
            sum_prog_mbps += prog_mbps;

            if (save_per_run) {
                csv_row_to_sd(true, run, "PROG_256B",
                              prog_hz, page_addr, 256u,
                              us_prog, prog_mbps, verr, sr1);
            }

            // -------- switch back to read clock --------
            spi_init(spi0, hz);
            cs_high();

            // -------- READ SEQ --------
            int64_t us_rseq = timed_read_seq(SCRATCH_BASE, READ_SEQ_SIZE);
            if (us_rseq < min_readseq_us) min_readseq_us = (double)us_rseq;
            if (us_rseq > max_readseq_us) max_readseq_us = (double)us_rseq;

            double rseq_mbps = _mbps(READ_SEQ_SIZE, us_rseq);
            sum_readseq_mbps += rseq_mbps;

            if (save_per_run) {
                csv_row_to_sd(true, run, "READ_SEQ",
                              hz, SCRATCH_BASE, READ_SEQ_SIZE,
                              us_rseq, rseq_mbps, 0, read_status(0x05));
            }

            // -------- READ RAND --------
            uint32_t seed = 0xC001D00Du ^ (uint32_t)run ^ (uint32_t)hz;
            double   rand_mbps_acc = 0.0;
            for (uint32_t i = 0; i < RAND_READ_ITERS; ++i) {
                uint32_t ra = 0;
                int64_t  us_rr = timed_read_rand256(&seed, &ra);

                if (us_rr < min_readrand_us) min_readrand_us = (double)us_rr;
                if (us_rr > max_readrand_us) max_readrand_us = (double)us_rr;

                double r_mb = _mbps(256, us_rr);
                rand_mbps_acc += r_mb;

                if (save_per_run) {
                    csv_row_to_sd(true, run, "READ_RAND",
                                  hz, ra, 256u,
                                  us_rr, r_mb, 0, read_status(0x05));
                }
            }
            sum_readrand_mbps += (rand_mbps_acc / (double)RAND_READ_ITERS);
        }

        // --- averages ---
        double avg_erase_ms      = (sum_erase_us / trials) / 1000.0;
        double avg_prog_mbps     =  sum_prog_mbps      / trials;
        double avg_readseq_mbps  =  sum_readseq_mbps   / trials;
        double avg_readrand_mbps =  sum_readrand_mbps  / trials;

        // --- latency ranges in nice units ---
        double min_erase_ms      = min_erase_us      / 1000.0;
        double max_erase_ms      = max_erase_us      / 1000.0;
        double min_prog_us_out   = min_prog_us;
        double max_prog_us_out   = max_prog_us;
        double min_rseq_ms       = min_readseq_us    / 1000.0;
        double max_rseq_ms       = max_readseq_us    / 1000.0;
        double min_rrand_us_out  = min_readrand_us;
        double max_rrand_us_out  = max_readrand_us;

        out("\r\n--- Averages over %d runs @ %u Hz ---\r\n", trials, hz);
        out("Erase 4KB: %.2f ms\r\n", avg_erase_ms);
        out("  Latency range: min %.2f ms, max %.2f ms\r\n",
            min_erase_ms, max_erase_ms);

        out("Write 256B: %.2f KB/s (%.3f MB/s)\r\n",
            avg_prog_mbps * 1024.0, avg_prog_mbps);
        out("  Latency range: min %.2f µs, max %.2f µs\r\n",
            min_prog_us_out, max_prog_us_out);

        out("Read %uKB (seq): %.2f KB/s (%.3f MB/s)\r\n",
            (unsigned)(READ_SEQ_SIZE / 1024),
            avg_readseq_mbps * 1024.0, avg_readseq_mbps);
        out("  Block latency range: min %.2f ms, max %.2f ms\r\n",
            min_rseq_ms, max_rseq_ms);

        out("Read 256B (rand avg): %.2f KB/s (%.3f MB/s)\r\n",
            avg_readrand_mbps * 1024.0, avg_readrand_mbps);
        out("  Per-transaction latency range: min %.2f µs, max %.2f µs\r\n",
            min_rrand_us_out, max_rrand_us_out);

        if (total_verify_errs) {
            out("ERROR: Verify failed — %u mismatched byte(s) across %d runs.\r\n",
                total_verify_errs, trials);
        }

        if (save_averages) {
            bench_csv_append_avg(jedec_hex, hz,
                                 avg_erase_ms,
                                 avg_prog_mbps    * 1024.0,
                                 avg_readseq_mbps * 1024.0,
                                 avg_readrand_mbps,
                                 total_verify_errs);
        }
    }

    if (save_averages) {
        bench_csv_end();
        out("\r\nSaved averages to %s\r\n", BENCH_PATH);
    }
}


// Wrapper for serial use (uses printf)
void run_fast_benchmark_web_safe(void) {
    run_fast_benchmark_with_output((printf_func_t)printf);
}

// Tiny wrappers so your main menu stays simple
void run_benchmarks(bool save_per_run) {
    run_benchmarks_with_trials(N_TRIALS, save_per_run, false);
}

void run_benchmarks_100(bool save_per_run) {
    run_benchmarks_with_trials(100, save_per_run, false);
}