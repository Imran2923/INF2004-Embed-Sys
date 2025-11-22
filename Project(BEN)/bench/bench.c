#include <stdio.h>
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"

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
#  define READ_SEQ_SIZE (256u * 1024u)   // 256 KB sequential read window
#endif

#ifndef RAND_READ_ITERS
#  define RAND_READ_ITERS 16u
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
#  define SCRATCH_BASE 0x000000u
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


static void wait_wip_clear_web_safe(void) {
    while (read_status(0x05) & 1) {
        tight_loop_contents();  // Busy wait - NO sleep_ms!
    }
}

static void sector_erase_4k_web_safe(uint32_t addr) {
    write_enable();
    uint8_t cmd[4] = {0x20, (uint8_t)(addr>>16), (uint8_t)(addr>>8), (uint8_t)addr};
    cs_low(); 
    spi_write_blocking(spi0, cmd, 4); 
    cs_high();
    wait_wip_clear_web_safe();  // NO sleep_ms!
}

static void page_program_web_safe(uint32_t addr, const uint8_t *data, uint32_t len) {
    write_enable();
    uint8_t hdr[4] = {0x02, (uint8_t)(addr>>16), (uint8_t)(addr>>8), (uint8_t)addr};
    cs_low(); 
    spi_write_blocking(spi0, hdr, 4);
    spi_write_blocking(spi0, data, (int)len); 
    cs_high();
    wait_wip_clear_web_safe();  // NO sleep_ms!
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

// Web-safe version of timed_erase_4k
static int64_t timed_erase_4k_web(uint32_t addr, uint8_t *sr1_end) {
    absolute_time_t t0 = get_absolute_time();
    sector_erase_4k_web_safe(addr);  // Use web-safe version
    int64_t us = absolute_time_diff_us(t0, get_absolute_time());
    if (sr1_end) *sr1_end = read_status(0x05);
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
static int64_t timed_prog_256_web(uint32_t addr, const uint8_t *page,
                                  uint32_t *verify_errs, uint8_t *sr1_end)
{
    addr &= ~0xFFu;

    // Program (web-safe)
    absolute_time_t t0 = get_absolute_time();
    page_program_web_safe(addr, page, 256);  // Use web-safe version
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

    // Erase whole scratch region once upfront to avoid stale data
    for (uint32_t a = SCRATCH_BASE; a < SCRATCH_BASE + SCRATCH_SIZE; a += 4096u) {
        uint8_t sr; (void)timed_erase_4k(a, &sr);
    }

    // Deterministic test pattern for programming
    uint8_t page[256]; for (int i=0;i<256;i++) page[i]=(uint8_t)i;

    // Optional: chip capability header
    uint8_t id[3]={0}; read_jedec_id(id);
    char jedec_hex[7];
    snprintf(jedec_hex, sizeof jedec_hex, "%02X%02X%02X", id[0], id[1], id[2]);
    uint8_t sfdp8[8]={0}; bool has_sfdp = read_sfdp_header(sfdp8);
    printf("# JEDEC=%02X %02X %02X  SFDP=%s\r\n",
           id[0], id[1], id[2], has_sfdp ? "OK" : "N/A");

    for (size_t fi = 0; fi < N_FREQS; ++fi) {
        uint32_t hz = SPI_FREQS[fi];
        spi_init(spi0, hz);

        double sum_erase_us = 0.0;
        double sum_prog_mbps = 0.0;
        double sum_readseq_mbps = 0.0;
        double sum_readrand_mbps = 0.0;
        uint32_t total_verify_errs = 0;

        for (int run = 1; run <= trials; ++run) {
            // rotate across sectors within scratch
            uint32_t sector_idx = (run-1) % (SCRATCH_SIZE/4096u);
            uint32_t era_addr   = SCRATCH_BASE + sector_idx*4096u;
            uint32_t page_addr  = era_addr; // first page in that sector

            // ERASE 4KB
            uint8_t  sr1 = 0;
            int64_t  us  = timed_erase_4k(era_addr, &sr1);
            sum_erase_us += (double)us;
            if (save_per_run)
                csv_row_to_sd(true, run, "ERASE_4K", hz, era_addr, 4096u, us, 0.0, 0, sr1);

            // PROGRAM 256B at safer clock to avoid unstable wiring issues
            spi_init(spi0, SAFE_PROG_HZ);
            cs_high();

            uint32_t verr = 0; sr1 = 0;
            us = timed_prog_256(page_addr, page, &verr, &sr1);
            total_verify_errs += verr;
            double prog_mbps = _mbps(256, us);
            sum_prog_mbps += prog_mbps;
            if (save_per_run)
                csv_row_to_sd(true, run, "PROG_256B", SAFE_PROG_HZ, page_addr, 256u, us, prog_mbps, verr, sr1);

            // switch back to benchmark frequency for reads
            spi_init(spi0, hz);
            cs_high();

            // READ SEQ over READ_SEQ_SIZE
            us = timed_read_seq(SCRATCH_BASE, READ_SEQ_SIZE);
            double rseq_mbps = _mbps(READ_SEQ_SIZE, us);
            sum_readseq_mbps += rseq_mbps;
            if (save_per_run)
                csv_row_to_sd(true, run, "READ_SEQ", hz, SCRATCH_BASE, READ_SEQ_SIZE, us, rseq_mbps, 0, read_status(0x05));

            // READ RAND: average over RAND_READ_ITERS, but log each sample as a separate measurement
            uint32_t seed = 0xC001D00Du ^ (uint32_t)run ^ (uint32_t)hz;
            double   rand_mbps_acc = 0.0;
            for (uint32_t i=0; i<RAND_READ_ITERS; ++i) {
                uint32_t ra=0; us = timed_read_rand256(&seed, &ra);
                double r_mb = _mbps(256, us);
                rand_mbps_acc += r_mb;
                if (save_per_run)
                    csv_row_to_sd(true, run, "READ_RAND", hz, ra, 256u, us, r_mb, 0, read_status(0x05));
            }
            sum_readrand_mbps += (rand_mbps_acc / (double)RAND_READ_ITERS);
        }

        // Pretty console summary for this SPI frequency
        double avg_erase_ms      = (sum_erase_us / trials) / 1000.0;
        double avg_prog_mbps     =  sum_prog_mbps / trials;
        double avg_readseq_mbps  =  sum_readseq_mbps / trials;
        double avg_readrand_mbps =  sum_readrand_mbps / trials;

        printf("\r\n=== Benchmark (avg over %d runs) ===\r\n", trials);
        printf("SPI clock: %u Hz\r\n\r\n", hz);
        printf("--- Averages over %d runs ---\r\n", trials);
        printf("Erase 4KB: %.2f ms\r\n", avg_erase_ms);
        printf("Write 256B: %.2f KB/s (%.3f MB/s)\r\n", avg_prog_mbps*1024.0, avg_prog_mbps);
        printf("Read %uKB (seq): %.2f KB/s (%.3f MB/s)\r\n",
               (unsigned)(READ_SEQ_SIZE/1024), avg_readseq_mbps*1024.0, avg_readseq_mbps);
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
            bench_csv_append_avg(jedec_hex,            // NEW: JEDEC in hex (e.g., "9D4013")
                         hz,
                         avg_erase_ms,
                         avg_prog_mbps * 1024.0,
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
    output_func("=== 100-Run Benchmark ===\r\n\r\n");
    
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
        
        double sum_erase_us = 0.0;
        double sum_prog_us = 0.0;
        double sum_read_us = 0.0;
        uint32_t total_errors = 0;
        
        for (int trial = 0; trial < TRIALS; ++trial) {
            // Progress indicator every 20 trials
            if (trial % 20 == 0 && trial > 0) {
                output_func("  Progress: %d/%d trials...\r\n", trial, TRIALS);
            }
            
            uint32_t sector_addr = SCRATCH_BASE + (trial % 64) * 4096;
            
            // ERASE (web-safe)
            spi_init(spi0, hz);
            cs_high();
            
            absolute_time_t t0 = get_absolute_time();
            write_enable();
            uint8_t cmd[4] = {0x20, (uint8_t)(sector_addr>>16), (uint8_t)(sector_addr>>8), (uint8_t)sector_addr};
            cs_low(); 
            spi_write_blocking(spi0, cmd, 4); 
            cs_high();
            while (read_status(0x05) & 1) { tight_loop_contents(); }
            int64_t us = absolute_time_diff_us(t0, get_absolute_time());
            sum_erase_us += (double)us;
            
            // PROGRAM (web-safe)
            spi_init(spi0, SAFE_PROG_HZ);
            cs_high();
            
            t0 = get_absolute_time();
            write_enable();
            uint8_t hdr[4] = {0x02, (uint8_t)(sector_addr>>16), (uint8_t)(sector_addr>>8), (uint8_t)sector_addr};
            cs_low(); 
            spi_write_blocking(spi0, hdr, 4);
            spi_write_blocking(spi0, page, 256); 
            cs_high();
            while (read_status(0x05) & 1) { tight_loop_contents(); }
            us = absolute_time_diff_us(t0, get_absolute_time());
            sum_prog_us += (double)us;
            
            // Verify
            uint8_t rb[256];
            read_data(sector_addr, rb, 256);
            for (int i = 0; i < 256; i++) {
                if (rb[i] != page[i]) total_errors++;
            }
            
            // READ (simplified - just read 4KB)
            spi_init(spi0, hz);
            cs_high();
            
            t0 = get_absolute_time();
            uint8_t buf[256];
            for (uint32_t off = 0; off < 4096; off += 256) {
                read_data(sector_addr + off, buf, 256);
            }
            us = absolute_time_diff_us(t0, get_absolute_time());
            sum_read_us += (double)us;
        }
        
        // Calculate averages
        double avg_erase_ms = (sum_erase_us / TRIALS) / 1000.0;
        double avg_prog_kbps = (256.0 * TRIALS) / (sum_prog_us / 1e6) / 1024.0;
        double avg_read_kbps = (4096.0 * TRIALS) / (sum_read_us / 1e6) / 1024.0;
        
        output_func("\r\n=== Results (avg over %d runs) ===\r\n", TRIALS);
        output_func("Erase 4KB: %.2f ms\r\n", avg_erase_ms);
        output_func("Write 256B: %.2f KB/s (%.3f MB/s)\r\n", avg_prog_kbps, avg_prog_kbps / 1024.0);
        output_func("Read 4KB: %.2f KB/s (%.3f MB/s)\r\n\r\n", avg_read_kbps, avg_read_kbps / 1024.0);
        
        if (total_errors > 0) {
            output_func("WARNING: %u verify errors!\r\n\r\n", total_errors);
        }
    }
    
    output_func("=== Complete ===\r\n");
}

void run_fast_benchmark_with_output(printf_func_t output_func) {
    output_func("=== Fast Benchmark ===\r\n\r\n");
    
    // Read chip ID
    uint8_t id[3] = {0};
    read_jedec_id(id);
    output_func("JEDEC: %02X %02X %02X\r\n\r\n", id[0], id[1], id[2]);
    
    // Test pattern
    uint8_t page[256];
    for (int i = 0; i < 256; i++) page[i] = (uint8_t)i;
    
    // Fast test: 2 frequencies, 2 trials each
    const uint32_t TEST_FREQS[] = {12000000u, 24000000u};
    const int TRIALS = 2;
    
    uint32_t total_errors = 0;
    
    for (size_t freq_idx = 0; freq_idx < 2; ++freq_idx) {
        uint32_t hz = TEST_FREQS[freq_idx];
        
        output_func("@ %u Hz:\r\n", hz);
        
        double sum_erase_us = 0.0;
        double sum_prog_us = 0.0;
        double sum_read_us = 0.0;
        
        for (int trial = 0; trial < TRIALS; ++trial) {
            // Use different 4KB sector for each test
            uint32_t sector_addr = SCRATCH_BASE + (freq_idx * TRIALS + trial) * 4096;
            
            // ERASE (web-safe version)
            spi_init(spi0, hz);
            cs_high();
            
            absolute_time_t t0 = get_absolute_time();
            // Inline web-safe erase
            write_enable();
            uint8_t cmd[4] = {0x20, (uint8_t)(sector_addr>>16), (uint8_t)(sector_addr>>8), (uint8_t)sector_addr};
            cs_low(); 
            spi_write_blocking(spi0, cmd, 4); 
            cs_high();
            while (read_status(0x05) & 1) { tight_loop_contents(); }
            int64_t us = absolute_time_diff_us(t0, get_absolute_time());
            sum_erase_us += (double)us;
            
            // PROGRAM (web-safe version)
            spi_init(spi0, SAFE_PROG_HZ);
            cs_high();
            
            t0 = get_absolute_time();
            write_enable();
            uint8_t hdr[4] = {0x02, (uint8_t)(sector_addr>>16), (uint8_t)(sector_addr>>8), (uint8_t)sector_addr};
            cs_low(); 
            spi_write_blocking(spi0, hdr, 4);
            spi_write_blocking(spi0, page, 256); 
            cs_high();
            while (read_status(0x05) & 1) { tight_loop_contents(); }
            us = absolute_time_diff_us(t0, get_absolute_time());
            sum_prog_us += (double)us;
            
            // Verify
            uint8_t rb[256];
            read_data(sector_addr, rb, 256);
            for (int i = 0; i < 256; i++) {
                if (rb[i] != page[i]) total_errors++;
            }
            
            // READ
            spi_init(spi0, hz);
            cs_high();
            
            t0 = get_absolute_time();
            uint8_t buf[256];
            for (uint32_t off = 0; off < 4096; off += 256) {
                read_data(sector_addr + off, buf, 256);
            }
            us = absolute_time_diff_us(t0, get_absolute_time());
            sum_read_us += (double)us;
        }
        
        // Calculate averages
        double avg_erase_ms = (sum_erase_us / TRIALS) / 1000.0;
        double avg_prog_kbps = (256.0 * TRIALS) / (sum_prog_us / 1e6) / 1024.0;
        double avg_read_kbps = (4096.0 * TRIALS) / (sum_read_us / 1e6) / 1024.0;
        
        output_func("  Erase:  %.2f ms\r\n", avg_erase_ms);
        output_func("  Write:  %.2f KB/s (%.3f MB/s)\r\n", avg_prog_kbps, avg_prog_kbps / 1024.0);
        output_func("  Read:   %.2f KB/s (%.3f MB/s)\r\n\r\n", avg_read_kbps, avg_read_kbps / 1024.0);
    }
    
    if (total_errors > 0) {
        output_func("WARNING: %u verify errors!\r\n", total_errors);
    }
    
    output_func("=== Complete ===\r\n");
}

void run_benchmarks_with_trials_web_safe(int trials, bool save_per_run, bool save_averages, printf_func_t output_func) {
    // Open averages CSV if requested
    if (save_averages) {
        FRESULT fr = bench_csv_begin();
        if (fr != FR_OK) {
            output_func("WARNING: benchmark.csv not opened; averages will not be saved.\r\n");
            save_averages = false;
        }
    }

    // Erase whole scratch region once upfront (web-safe)
    for (uint32_t a = SCRATCH_BASE; a < SCRATCH_BASE + SCRATCH_SIZE; a += 4096u) {
        uint8_t sr; 
        (void)timed_erase_4k_web(a, &sr);  // Web-safe erase
    }

    // Deterministic test pattern for programming
    uint8_t page[256]; 
    for (int i=0; i<256; i++) page[i]=(uint8_t)i;

    // Optional: chip capability header
    uint8_t id[3]={0}; 
    read_jedec_id(id);
    char jedec_hex[7];
    snprintf(jedec_hex, sizeof jedec_hex, "%02X%02X%02X", id[0], id[1], id[2]);
    uint8_t sfdp8[8]={0}; 
    bool has_sfdp = read_sfdp_header(sfdp8);
    output_func("# JEDEC=%02X %02X %02X  SFDP=%s\r\n",
           id[0], id[1], id[2], has_sfdp ? "OK" : "N/A");

    for (size_t fi = 0; fi < N_FREQS; ++fi) {
        uint32_t hz = SPI_FREQS[fi];
        spi_init(spi0, hz);
        cs_high();

        double sum_erase_us = 0.0;
        double sum_prog_mbps = 0.0;
        double sum_readseq_mbps = 0.0;
        double sum_readrand_mbps = 0.0;
        uint32_t total_verify_errs = 0;

        for (int run = 1; run <= trials; ++run) {
            // Progress indicator
            if (run % 10 == 0) {
                output_func("  Progress: %d/%d trials...\r\n", run, trials);
            }
            
            // rotate across sectors within scratch
            uint32_t sector_idx = (run-1) % (SCRATCH_SIZE/4096u);
            uint32_t era_addr   = SCRATCH_BASE + sector_idx*4096u;
            uint32_t page_addr  = era_addr; // first page in that sector

            // ERASE 4KB (web-safe)
            uint8_t  sr1 = 0;
            int64_t  us  = timed_erase_4k_web(era_addr, &sr1);
            sum_erase_us += (double)us;
            if (save_per_run)
                csv_row_to_sd(true, run, "ERASE_4K", hz, era_addr, 4096u, us, 0.0, 0, sr1);

            // PROGRAM 256B at safer clock (web-safe)
            spi_init(spi0, SAFE_PROG_HZ);
            cs_high();

            uint32_t verr = 0; sr1 = 0;
            us = timed_prog_256_web(page_addr, page, &verr, &sr1);
            total_verify_errs += verr;
            double prog_mbps = _mbps(256, us);
            sum_prog_mbps += prog_mbps;
            if (save_per_run)
                csv_row_to_sd(true, run, "PROG_256B", SAFE_PROG_HZ, page_addr, 256u, us, prog_mbps, verr, sr1);

            // switch back to benchmark frequency for reads
            spi_init(spi0, hz);
            cs_high();

            // READ SEQ over READ_SEQ_SIZE (read_data is already safe)
            us = timed_read_seq(SCRATCH_BASE, READ_SEQ_SIZE);
            double rseq_mbps = _mbps(READ_SEQ_SIZE, us);
            sum_readseq_mbps += rseq_mbps;
            if (save_per_run)
                csv_row_to_sd(true, run, "READ_SEQ", hz, SCRATCH_BASE, READ_SEQ_SIZE, us, rseq_mbps, 0, read_status(0x05));

            // READ RAND
            uint32_t seed = 0xC001D00Du ^ (uint32_t)run ^ (uint32_t)hz;
            double   rand_mbps_acc = 0.0;
            for (uint32_t i=0; i<RAND_READ_ITERS; ++i) {
                uint32_t ra=0; 
                us = timed_read_rand256(&seed, &ra);
                double r_mb = _mbps(256, us);
                rand_mbps_acc += r_mb;
                if (save_per_run)
                    csv_row_to_sd(true, run, "READ_RAND", hz, ra, 256u, us, r_mb, 0, read_status(0x05));
            }
            sum_readrand_mbps += (rand_mbps_acc / (double)RAND_READ_ITERS);
        }

        // Pretty console summary for this SPI frequency
        double avg_erase_ms      = (sum_erase_us / trials) / 1000.0;
        double avg_prog_mbps     =  sum_prog_mbps / trials;
        double avg_readseq_mbps  =  sum_readseq_mbps / trials;
        double avg_readrand_mbps =  sum_readrand_mbps / trials;

        output_func("\r\n=== Benchmark (avg over %d runs) ===\r\n", trials);
        output_func("SPI clock: %u Hz\r\n\r\n", hz);
        output_func("--- Averages over %d runs ---\r\n", trials);
        output_func("Erase 4KB: %.2f ms\r\n", avg_erase_ms);
        output_func("Write 256B: %.2f KB/s (%.3f MB/s)\r\n", avg_prog_mbps*1024.0, avg_prog_mbps);
        output_func("Read %uKB (seq): %.2f KB/s (%.3f MB/s)\r\n",
               (unsigned)(READ_SEQ_SIZE/1024), avg_readseq_mbps*1024.0, avg_readseq_mbps);
        if (total_verify_errs) {
            output_func("ERROR: Verify failed — %u mismatched byte(s) across %d run(s).\r\n",
                   total_verify_errs, trials);
        }

        if (save_averages) {
            bench_csv_append_avg(jedec_hex, hz, avg_erase_ms,
                                avg_prog_mbps * 1024.0,
                                avg_readseq_mbps * 1024.0,
                                avg_readrand_mbps,
                                total_verify_errs);
        }
    }

    if (save_averages) {
        bench_csv_end();
        output_func("Saved averages to %s\r\n", BENCH_PATH);
    }
}

// Wrapper for serial use (uses printf)
void run_fast_benchmark_web_safe(void) {
    run_fast_benchmark_with_output(printf);
}

// Tiny wrappers so your main menu stays simple
void run_benchmarks(bool save_per_run) {
    run_benchmarks_with_trials(N_TRIALS, save_per_run, false);
}

void run_benchmarks_100(bool save_per_run) {
    run_benchmarks_with_trials(100, save_per_run, false);
}