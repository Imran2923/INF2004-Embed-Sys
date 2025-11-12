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

// Tiny wrappers so your main menu stays simple
void run_benchmarks(bool save_per_run) {
    run_benchmarks_with_trials(N_TRIALS, save_per_run, false);
}

void run_benchmarks_100(bool save_per_run) {
    run_benchmarks_with_trials(100, save_per_run, false);
}
