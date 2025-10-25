// pico_spi_flash_sd_menu.c
// SPI flash demo + Benchmark + SD CSV logging.
// Menu:
// 1: Run Benchmark
// 2: Run Test Connection
// 3: Run Benchmark and Save Results to CSV
// 4: Read Results
//
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

/* =================== SPI FLASH CONFIG =================== */
#define PIN_SCK   2
#define PIN_MOSI  3
#define PIN_MISO  4
#define PIN_CS    6

/* Trials and SPI freqs (FR-5, FR-6) */
#define N_TRIALS        10
static const uint32_t SPI_FREQS[] = { 12000000u, 24000000u, 36000000u };
#define N_FREQS (sizeof(SPI_FREQS)/sizeof(SPI_FREQS[0]))

/* Scratch region (FR-10) — stay inside this window */
#define SCRATCH_BASE     0x000000u
#define SCRATCH_SIZE     (384u * 1024u)     /* 384 KB safe area */

/* Work sizes (FR-3) */
#define READ_SEQ_SIZE    (256u * 1024u)     /* ≥256 KB contiguous */
#define RAND_READ_ITERS  64u                /* number of 256B random reads per run */

/* Timeouts (FR-14) */
#define TOUT_PROG_US     20000              /* 20 ms per 256B page */
#define TOUT_ERASE_US    500000             /* 500 ms per 4KB sector */

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

/* =================== SPI FLASH HELPERS =================== */

static inline void cs_low(void){  gpio_put(PIN_CS, 0); }
static inline void cs_high(void){ gpio_put(PIN_CS, 1); }
static FATFS g_fs;
static FIL   g_csv;
static bool  g_csv_open = false;
#define CSV_PATH "0:/pico_test/results.csv"

static void release_from_dp(void){
    uint8_t cmd = 0xAB;
    cs_low(); spi_write_blocking(spi0, &cmd, 1); cs_high();
    sleep_ms(1);
}

/* Small utils */
static inline double _mbps(size_t bytes, int64_t us){
    if (us <= 0) return 0.0;
    return (bytes / (1024.0 * 1024.0)) / (us / 1e6);
}
static uint32_t _xors(uint32_t *s){ *s^=*s<<13; *s^=*s>>17; *s^=*s<<5; return *s; }

/* Mount card and open CSV (append). If the file is empty, write header once. */
static FRESULT csv_begin(void) {
    FRESULT fr = f_mount(&g_fs, "0:", 1);
    if (fr != FR_OK) { printf("f_mount err=%d\r\n", fr); return fr; }

    // ensure folder exists (ok if already present)
    f_mkdir("0:/pico_test");

    fr = f_open(&g_csv, CSV_PATH, FA_OPEN_ALWAYS | FA_WRITE);
    if (fr != FR_OK) { printf("f_open err=%d\r\n", fr); return fr; }

    // Header only if file is empty
    if (f_size(&g_csv) == 0) {
        const char *hdr =
            "run,op,spi_hz,addr,bytes,duration_us,mbps,verify_errors,status1_end\r\n";
        UINT bw = 0; fr = f_write(&g_csv, hdr, (UINT)strlen(hdr), &bw);
        if (fr != FR_OK || bw != strlen(hdr)) { printf("write hdr err=%d\r\n", fr); }
        f_sync(&g_csv);
    }

    // Seek to end for appending rows
    f_lseek(&g_csv, f_size(&g_csv));
    g_csv_open = true;
    return FR_OK;
}

static void csv_append_line(const char *line) {
    if (!g_csv_open) return;
    UINT bw = 0; FRESULT fr = f_write(&g_csv, line, (UINT)strlen(line), &bw);
    if (fr != FR_OK || bw != strlen(line)) printf("csv append err=%d\r\n", fr);
}

static void csv_end(void) {
    if (g_csv_open) {
        f_sync(&g_csv);
        f_close(&g_csv);
        f_unmount("0:");
        g_csv_open = false;
    }
}

/* Format a CSV row into a buffer and (optionally) append to SD */
static inline void csv_row_to_sd(bool save_csv, int run, const char *op, uint32_t spi_hz,
                                 uint32_t addr, uint32_t bytes, int64_t dur_us,
                                 double mbps, uint32_t verify_errors, uint8_t sr1_end) {
    if (!save_csv) return;
    char line[192];
    int n = snprintf(line, sizeof line,
        "%d,%s,%u,0x%06X,%u,%lld,%.6f,%u,%02X\r\n",
        run, op, spi_hz, addr, bytes, (long long)dur_us, mbps, verify_errors, sr1_end);
    if (n > 0 && n < (int)sizeof line) csv_append_line(line);
}


/* FR-2: SFDP (0x5A) – read first 8B header; returns true if "SFDP" */
static bool read_sfdp_header(uint8_t out8[8]){
    uint8_t cmd[5] = {0x5A, 0x00, 0x00, 0x00, 0x00};  // addr=0, 1 dummy
    cs_low();
    spi_write_blocking(spi0, cmd, 5);
    spi_read_blocking(spi0, 0x00, out8, 8);
    cs_high();
    return (out8[0]==0x53 && out8[1]==0x46 && out8[2]==0x44 && out8[3]==0x50); // "SFDP"
}

static void read_jedec_id(uint8_t id[3]) {
    uint8_t tx[4] = {0x9F, 0, 0, 0};
    uint8_t rx[4] = {0};
    cs_low();  sleep_us(2);
    spi_write_read_blocking(spi0, tx, rx, 4);
    sleep_us(2); cs_high();
    id[0] = rx[1]; id[1] = rx[2]; id[2] = rx[3];
}

static uint8_t read_status(uint8_t cmd) {   // 0x05 (SR1) or 0x35 (SR2)
    uint8_t tx[2] = {cmd, 0}, rx[2] = {0};
    cs_low();  spi_write_read_blocking(spi0, tx, rx, 2);  cs_high();
    return rx[1];
}

// 0x03 legacy READ (blocking)
static void read_data(uint32_t addr, uint8_t *buf, size_t len){
    uint8_t hdr[4] = {
        0x03,
        (uint8_t)(addr >> 16),
        (uint8_t)(addr >> 8),
        (uint8_t)addr
    };
    cs_low();
    spi_write_blocking(spi0, hdr, 4);
    spi_read_blocking(spi0, 0x00, buf, (int)len);
    cs_high();
}

static void write_enable(void) {
    uint8_t cmd = 0x06;
    cs_low();  spi_write_blocking(spi0, &cmd, 1);  cs_high();
}

static void wait_wip_clear(void) {          // poll SR1.WIP
    while (read_status(0x05) & 0x01) { sleep_ms(1); }
}

/* ERASE_4K with timeout; returns duration us; writes SR1 at end (FR-14) */
static int64_t timed_erase_4k(uint32_t addr, uint8_t *sr1_end){
    absolute_time_t t0 = get_absolute_time();
    write_enable();
    uint8_t cmd[4] = {0x20, (uint8_t)(addr>>16), (uint8_t)(addr>>8), (uint8_t)addr};
    cs_low(); spi_write_blocking(spi0, cmd, 4); cs_high();

    absolute_time_t deadline = delayed_by_us(t0, TOUT_ERASE_US);
    while (read_status(0x05) & 0x01){
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) break; // timeout
    }
    *sr1_end = read_status(0x05);
    return absolute_time_diff_us(t0, get_absolute_time());
}

/* Program exactly 256B, then verify and count mismatches (FR-3, FR-7, FR-8, FR-14) */
static int64_t timed_prog_256(uint32_t addr, const uint8_t *page,
                              uint32_t *verify_errs, uint8_t *sr1_end){
    uint8_t hdr[4] = {0x02, (uint8_t)(addr>>16), (uint8_t)(addr>>8), (uint8_t)addr};
    absolute_time_t t0 = get_absolute_time();
    write_enable();
    cs_low(); spi_write_blocking(spi0, hdr, 4); spi_write_blocking(spi0, page, 256); cs_high();

    absolute_time_t deadline = delayed_by_us(t0, TOUT_PROG_US);
    while (read_status(0x05) & 0x01){
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) break; // timeout
    }
    *sr1_end = read_status(0x05);
    int64_t us = absolute_time_diff_us(t0, get_absolute_time());

    /* Verify (FR-7/8) */
    uint8_t rb[256]; read_data(addr, rb, 256);
    uint32_t e=0; for (int i=0;i<256;i++) if (rb[i]!=page[i]) e++;
    *verify_errs = e;
    return us;
}

/* READ_SEQ (0x03) over 'len' bytes; returns duration us (FR-3/4) */
static int64_t timed_read_seq(uint32_t addr, size_t len){
    uint8_t hdr[4] = {0x03, (uint8_t)(addr>>16), (uint8_t)(addr>>8), (uint8_t)addr};
    absolute_time_t t0 = get_absolute_time();
    cs_low(); spi_write_blocking(spi0, hdr, 4);
    uint8_t buf[512];
    size_t left = len;
    while (left){
        size_t n = left > sizeof(buf) ? sizeof(buf) : left;
        spi_read_blocking(spi0, 0x00, buf, (int)n);
        left -= n;
    }
    cs_high();
    return absolute_time_diff_us(t0, get_absolute_time());
}

/* READ_RAND: one 256B read at random page within SCRATCH (FR-3) */
static int64_t timed_read_rand256(uint32_t *seed, uint32_t *addr_out){
    uint32_t pages = SCRATCH_SIZE / 256u;
    uint32_t pick  = _xors(seed) % pages;
    uint32_t addr  = SCRATCH_BASE + pick*256u;
    if (addr_out) *addr_out = addr;

    uint8_t hdr[4] = {0x03, (uint8_t)(addr>>16), (uint8_t)(addr>>8), (uint8_t)addr};
    uint8_t rb[256];
    absolute_time_t t0 = get_absolute_time();
    cs_low(); spi_write_blocking(spi0, hdr, 4);
    spi_read_blocking(spi0, 0x00, rb, 256);
    cs_high();
    return absolute_time_diff_us(t0, get_absolute_time());
}

static void sector_erase_4k(uint32_t addr) { // 0x20 + 24-bit addr
    write_enable();
    uint8_t cmd[4] = {0x20, (uint8_t)(addr>>16), (uint8_t)(addr>>8), (uint8_t)addr};
    cs_low();  spi_write_blocking(spi0, cmd, 4);  cs_high();
    wait_wip_clear();
}

static void page_program(uint32_t addr, const uint8_t *data, size_t len) {
    // len <= 256 and must not cross a 256B page boundary
    write_enable();
    uint8_t hdr[4] = {0x02, (uint8_t)(addr>>16), (uint8_t)(addr>>8), (uint8_t)addr};
    cs_low();
    spi_write_blocking(spi0, hdr, 4);
    spi_write_blocking(spi0, data, (int)len);
    cs_high();
    wait_wip_clear();
}

// Many SPI NORs support JEDEC soft reset: 0x66 (Reset Enable), then 0x99 (Reset)
static void flash_soft_reset(void){
    uint8_t cmd;
    cmd = 0x66; cs_low(); spi_write_blocking(spi0, &cmd, 1); cs_high();
    sleep_us(2); // tSHSL tiny gap
    cmd = 0x99; cs_low(); spi_write_blocking(spi0, &cmd, 1); cs_high();
    // give the device time to internally reset (datasheets: ~30–50us usually)
    sleep_ms(1);
}

// Some parts also wake from deep power-down with 0xAB (no harm if not in DPD)
static void flash_release_from_dp(void){
    uint8_t cmd = 0xAB;
    cs_low(); spi_write_blocking(spi0, &cmd, 1); cs_high();
    sleep_us(50);
}

// Safe frequency to fall back to after benchmarks
#define SPI_FREQ_SAFE 4000000u  // 4 MHz is conservative for shaky wiring

static void flash_recover_to_safe_mode(void){
    // Try both: release-from-DP and soft-reset (harmless if not needed)
    flash_release_from_dp();
    flash_soft_reset();
    // Drop SPI speed to something very safe for JEDEC ID reads
    spi_init(spi0, SPI_FREQ_SAFE);
    cs_high();
    sleep_ms(1);
}



/* =================== MENU ACTIONS =================== */

static void action_test_connection(void) {
    printf("\r\n=== Test Connection ===\r\n");

    uint8_t id[3] = {0};
    read_jedec_id(id);
    printf("JEDEC ID: %02X %02X %02X\r\n", id[0], id[1], id[2]);

    uint8_t sr1 = read_status(0x05);
    uint8_t sr2 = read_status(0x35);
    printf("SR1: %02X  (WIP=bit0, WEL=bit1)\r\n", sr1);
    printf("SR2: %02X\r\n", sr2);

    const uint32_t test_addr = 0x000000;
    const uint8_t msg[] = "Hello, Flash!\r\n";

    printf("Erasing 4K sector @0x%06lX...\r\n", (unsigned long)test_addr);
    sector_erase_4k(test_addr);

    printf("Programming %u bytes...\r\n", (unsigned)sizeof(msg));
    page_program(test_addr, msg, sizeof(msg));

    uint8_t rb[32] = {0};
    read_data(test_addr, rb, sizeof(rb));
    printf("Read-back (32B @0x000000):\r\n");
    for (int i = 0; i < (int)sizeof(rb); ++i) {
        printf("%02X ", rb[i]);
        if ((i & 15) == 15) printf("\r\n");
    }
    printf("\r\n");

    // Verify
    int errors = 0;
    for (size_t i = 0; i < sizeof(msg); i++) {
        if (rb[i] != msg[i]) errors++;
    }
    printf("Verification %s, errors = %d\r\n", errors ? "FAILED" : "PASSED", errors);

    uint8_t sr_end = read_status(0x05);
    printf("Status after program: %02X\r\n", sr_end);

    printf("=== Done ===\r\n");
}


typedef struct {
    double avg_erase_ms;
    double avg_write_kbs;
    double avg_read_kbs;
} bench_result_t;

// === Benchmark runner: prints human-readable summary;
// Run with a chosen number of trials; if save_csv==true, also writes per-run rows to SD
static void run_benchmarks_with_trials(int trials, bool save_csv) {
    if (save_csv) {
        FRESULT fr = csv_begin();
        if (fr != FR_OK) { printf("CSV open failed (%d), continuing without save.\r\n", fr); save_csv = false; }
    }

    uint8_t id[3]={0}; read_jedec_id(id);
    uint8_t sfdp8[8]={0}; bool has_sfdp = read_sfdp_header(sfdp8);
    printf("# JEDEC=%02X %02X %02X  SFDP=%s\r\n",
           id[0], id[1], id[2], has_sfdp ? "OK" : "N/A");

    for (uint32_t a = SCRATCH_BASE; a < SCRATCH_BASE + SCRATCH_SIZE; a += 4096) {
        uint8_t sr; (void)timed_erase_4k(a, &sr);
    }
    uint8_t page[256]; for (int i=0;i<256;i++) page[i]=(uint8_t)i;

    for (size_t fi = 0; fi < N_FREQS; ++fi) {
        uint32_t hz = SPI_FREQS[fi];
        spi_init(spi0, hz); cs_high();

        double sum_erase_us = 0.0;
        double sum_prog_mbps = 0.0, sum_readseq_mbps = 0.0, sum_readrand_mbps = 0.0;
        uint32_t total_verify_errs = 0;

        for (int run = 1; run <= trials; ++run) {
            uint32_t sector_idx = (run-1) % (SCRATCH_SIZE/4096u);
            uint32_t era_addr   = SCRATCH_BASE + sector_idx*4096u;

            uint8_t  sr1 = 0;
            int64_t  us  = timed_erase_4k(era_addr, &sr1);
            sum_erase_us += (double)us;
            csv_row_to_sd(save_csv, run, "ERASE_4K", hz, era_addr, 4096u, us, 0.0, 0, sr1);

            uint32_t verr = 0; sr1 = 0;
            us = timed_prog_256(era_addr, page, &verr, &sr1);
            total_verify_errs += verr;
            double prog_mbps = _mbps(256, us);
            sum_prog_mbps += prog_mbps;
            csv_row_to_sd(save_csv, run, "PROG_256B", hz, era_addr, 256u, us, prog_mbps, verr, sr1);

            us = timed_read_seq(SCRATCH_BASE, READ_SEQ_SIZE);
            double rseq_mbps = _mbps(READ_SEQ_SIZE, us);
            sum_readseq_mbps += rseq_mbps;
            csv_row_to_sd(save_csv, run, "READ_SEQ", hz, SCRATCH_BASE, READ_SEQ_SIZE, us, rseq_mbps, 0, read_status(0x05));

            uint32_t seed = 0xC001D00Du ^ (uint32_t)run ^ (uint32_t)hz;
            double   rand_mbps_acc = 0.0;
            for (uint32_t i=0; i<RAND_READ_ITERS; ++i) {
                uint32_t ra=0; us = timed_read_rand256(&seed, &ra);
                double r_mb = _mbps(256, us);
                rand_mbps_acc += r_mb;
                csv_row_to_sd(save_csv, run, "READ_RAND", hz, ra, 256u, us, r_mb, 0, read_status(0x05));
            }
            sum_readrand_mbps += (rand_mbps_acc / (double)RAND_READ_ITERS);
        }

        double avg_erase_ms      = (sum_erase_us / trials) / 1000.0;
        double avg_prog_mbps     =  sum_prog_mbps / trials;
        double avg_readseq_mbps  =  sum_readseq_mbps / trials;

        printf("\r\n=== Benchmark (avg over %d runs) ===\r\n", trials);
        printf("SPI clock: %u Hz\r\n\r\n", hz);
        printf("--- Averages over %d runs ---\r\n", trials);
        printf("Erase 4KB: %.2f ms\r\n", avg_erase_ms);
        printf("Write 256B: %.2f KB/s (%.3f MB/s)\r\n", avg_prog_mbps*1024.0, avg_prog_mbps);
        printf("Read %uKB (seq): %.2f KB/s (%.3f MB/s)\r\n",
               (unsigned)(READ_SEQ_SIZE/1024), avg_readseq_mbps*1024.0, avg_readseq_mbps);
        if (total_verify_errs)
            printf("Note: verify_errors accumulated = %u\r\n", total_verify_errs);
    }

    if (save_csv) { csv_end(); printf("Saved per-run rows to %s\r\n", CSV_PATH); }
    flash_recover_to_safe_mode();
}

/* =================== SD / CSV HELPERS =================== */

// at top-level (global scope), once:
static FATFS g_fs;

// mounts 0: and ensures 0:/pico_test exists
static FRESULT ensure_sd_and_folder(void) {
    FRESULT fr = f_mount(&g_fs, "0:", 1);
    if (fr != FR_OK) { printf("f_mount error: %d\r\n", fr); return fr; }
    f_mkdir("0:/pico_test"); // ok if already exists
    return FR_OK;
}

static FRESULT append_csv_row(const bench_result_t *r) {
    FRESULT fr = ensure_sd_and_folder();
    if (fr != FR_OK) return fr;

    FIL file; UINT bw;
    fr = f_open(&file, "0:/pico_test/results.csv", FA_WRITE | FA_OPEN_ALWAYS);
    if (fr != FR_OK) { printf("open CSV err=%d\r\n", fr); goto out_umount; }

    if (f_size(&file) == 0) {
        const char *hdr = "run_ms_since_boot,avg_erase_ms,avg_write_kBps,avg_read_kBps\r\n";
        fr = f_write(&file, hdr, (UINT)strlen(hdr), &bw);
        if (fr != FR_OK || bw != strlen(hdr)) { printf("CSV header write err=%d\r\n", fr); goto close_file; }
        f_sync(&file);
    }

    f_lseek(&file, f_size(&file));
    uint64_t ms_since_boot = to_ms_since_boot(get_absolute_time());
    char line[160];
    int n = snprintf(line, sizeof line, "%llu,%.3f,%.3f,%.3f\r\n",
                     (unsigned long long)ms_since_boot,
                     r->avg_erase_ms, r->avg_write_kbs, r->avg_read_kbs);
    fr = f_write(&file, line, (UINT)n, &bw);
    if (fr != FR_OK || bw != (UINT)n) { printf("CSV append err=%d\r\n", fr); goto close_file; }
    f_sync(&file);

close_file:
    f_close(&file);
out_umount:
    f_unmount("0:");
    return fr;
}

// Keep old call sites working:
static inline void run_benchmarks(bool save_csv) {
    run_benchmarks_with_trials(N_TRIALS, save_csv);
}
// New convenience for the demo:
static inline void run_benchmarks_100(bool save_csv) {
    run_benchmarks_with_trials(100, save_csv);
}

static FRESULT print_csv(void) {
    FRESULT fr = ensure_sd_and_folder();
    if (fr != FR_OK) return fr;

    FIL file; UINT br; char buffer[256];
    fr = f_open(&file, "0:/pico_test/results.csv", FA_READ);
    if (fr != FR_OK) { printf("No CSV yet (%d). Run option 3 first.\r\n", fr); goto out_umount; }

    printf("\r\n--- 0:/pico_test/results.csv ---\r\n");
    do {
        fr = f_read(&file, buffer, sizeof(buffer) - 1, &br);
        if (fr != FR_OK) { printf("Read error: %d\r\n", fr); break; }
        buffer[br] = '\0';
        printf("%s", buffer);
    } while (br > 0);
    printf("\r\n--- End CSV ---\r\n");

    f_close(&file);
out_umount:
    f_unmount("0:");
    return fr;
}


/* =================== SERIAL MENU =================== */

static void print_menu(void){
    printf("\r\n=============================\r\n");
    printf("1: Run Benchmark\r\n");
    printf("2: Run Test Connection\r\n");
    printf("3: Run Benchmark and Save Results to CSV\r\n");
    printf("4: Read Results\r\n");
    printf("5: Run Benchmark (100-run demo summary)\r\n");
    printf("q: Quit (stop menu)\r\n");
    printf("=============================\r\n");
    printf("Enter choice: ");
    fflush(stdout);
}

static int get_choice_blocking(void){
    int ch = PICO_ERROR_TIMEOUT;
    while (ch == PICO_ERROR_TIMEOUT) {
        ch = getchar_timeout_us(1000 * 1000); // 1s poll
    }
    if (ch == '\r' || ch == '\n') return get_choice_blocking();
    return ch;
}

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

    while (true) {
        print_menu();
        int c = get_choice_blocking();
        printf("%c\r\n", c);

        if (c == '1') {
            // Run benchmarks, log to serial
            run_benchmarks(false);
        } else if (c == '2') {
            // Simple connection + verify test
            action_test_connection();
        } else if (c == '3') {
            // Run benchmarks and also save results to CSV on SD (if implemented inside csv_row)
            run_benchmarks(false);
        } else if (c == '4') {
            // Read back CSV file from SD card (if implemented)
            (void)print_csv();
        } else if (c == '5') {
            // Read back CSV file from SD card (if implemented)
             run_benchmarks_100(false);
        } else if (c == 'q' || c == 'Q') {
            printf("Exiting menu. Reset board to reopen.\r\n");
            break;
        } else {
            printf("Unknown choice. Try again.\r\n");
        }
    }

    while (1) { sleep_ms(1000); }
}

