#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "ff.h"
#include "csvlog.h"

// Keep FatFs state here (single module owns these)
static FATFS g_fs;              // filesystem
static FIL   g_csv;             // results.csv
static bool  g_csv_open = false;

static FIL   g_bench_csv;       // benchmark.csv (averages)
static bool  g_bench_open = false;
static DWORD g_last_session_offset = 0; 

static void _friendly_mount_error(FRESULT fr){
    if (fr == FR_NOT_READY) {
        printf("ERROR: No SD card detected. Insert a microSD card and try again.\r\n");
    } else {
        printf("ERROR: SD mount failed (FatFs err=%d). Check wiring/format.\r\n", fr);
    }
}

static FRESULT ensure_sd_dir(void){
    FRESULT fr = f_mount(&g_fs, "0:", 1);
    if (fr != FR_OK) return fr;
    f_mkdir("0:/pico_test");   // ok if it already exists
    return FR_OK;
}


// ---------------- results.csv (per-measurement rows) ----------------

FRESULT csv_begin(void) {
    FRESULT fr = f_mount(&g_fs, "0:", 1);
    if (fr != FR_OK) { _friendly_mount_error(fr); return fr; }

    f_mkdir("0:/pico_test"); // OK if exists

    fr = f_open(&g_csv, CSV_PATH, FA_OPEN_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        printf("ERROR: Could not open %s (err=%d).\r\n", CSV_PATH, fr);
        return fr;
    }

    if (f_size(&g_csv) == 0) {
        const char *hdr = "run,op,spi_hz,addr,bytes,duration_us,mbps,verify_errors,status1_end\r\n";
        UINT bw=0; fr = f_write(&g_csv, hdr, (UINT)strlen(hdr), &bw);
        if (fr != FR_OK || bw != (UINT)strlen(hdr)) {
            printf("ERROR: Failed writing results header (err=%d).\r\n", fr);
        }
        f_sync(&g_csv);
    }
    f_lseek(&g_csv, f_size(&g_csv)); // append
    g_csv_open = true;
    return FR_OK;
}

void csv_end(void) {
    if (!g_csv_open) return;
    f_sync(&g_csv);
    f_close(&g_csv);
    g_csv_open = false;
}

static void _csv_append_line(const char *line){
    if (!g_csv_open) return;
    UINT bw=0; FRESULT fr = f_write(&g_csv, line, (UINT)strlen(line), &bw);
    if (fr != FR_OK || bw != (UINT)strlen(line)) {
        printf("ERROR: results.csv append err=%d (bw=%u)\r\n", fr, (unsigned)bw);
    }
}

void csv_row_to_sd(bool save, int run, const char* op, uint32_t hz,
                   uint32_t addr, uint32_t bytes, int64_t dur_us,
                   double mbps, uint32_t verify_errors, uint8_t sr1_end)
{
    if (!save || !g_csv_open) return;
    char line[192];
    int n = snprintf(line, sizeof line, "%d,%s,%u,0x%06X,%u,%lld,%.6f,%u,%02X\r\n",
                     run, op, hz, addr, bytes, (long long)dur_us, mbps, verify_errors, sr1_end);
    if (n > 0 && n < (int)sizeof line) _csv_append_line(line);
}

// Mark the start of a saved test; return file offset to allow truncation
DWORD csv_mark_session_start(void) {
    if (!g_csv_open) return 0;
    DWORD pos = f_size(&g_csv);  // position BEFORE writing marker
    char line[64];
    uint32_t ms = to_ms_since_boot(get_absolute_time());
    int n = snprintf(line, sizeof line, "# SESSION_START %lu\r\n", (unsigned long)ms);
    if (n > 0 && n < (int)sizeof line) _csv_append_line(line);
    f_sync(&g_csv);
    return pos;
}

// Scan for the last marker and truncate file back to it
FRESULT csv_erase_last_session(void) {
    FRESULT fr = f_mount(&g_fs, "0:", 1);
    if (fr != FR_OK) { _friendly_mount_error(fr); return fr; }

    FIL f; fr = f_open(&f, CSV_PATH, FA_READ | FA_WRITE);
    if (fr != FR_OK) {
        printf("No CSV found (%s), err=%d\r\n", CSV_PATH, fr);
        return fr;
    }

    char line[256];
    DWORD last_marker_pos = 0;

    // skip header
    (void)f_gets(line, sizeof line, &f);
    for (;;) {
        DWORD pos = f_tell(&f);
        TCHAR *s = f_gets(line, sizeof line, &f);
        if (!s) break;
        if (line[0]=='#' && strstr(line, "SESSION_START")) {
            last_marker_pos = pos;
        }
    }

    if (last_marker_pos == 0) {
        printf("No session marker found; nothing to erase.\r\n");
        f_close(&f);
        return FR_OK;
    }

    fr = f_lseek(&f, last_marker_pos);
    if (fr == FR_OK) fr = f_truncate(&f);
    f_sync(&f);
    f_close(&f);
    printf("Erased last session starting at byte %lu.\r\n", (unsigned long)last_marker_pos);
    return fr;
}

FRESULT print_csv(void) {
    FRESULT fr = f_mount(&g_fs, "0:", 1);
    if (fr != FR_OK) { _friendly_mount_error(fr); return fr; }

    FIL f; fr = f_open(&f, CSV_PATH, FA_READ);
    if (fr != FR_OK) { printf("Open %s err=%d\r\n", CSV_PATH, fr); return fr; }

    char line[256];
    while (f_gets(line, sizeof line, &f)) {
        printf("%s", line); // line already has \r\n from file
    }
    f_close(&f);
    return FR_OK;
}

// ---------------- benchmark.csv (averages) ----------------

FRESULT bench_csv_begin(void) {
    // Do NOT f_mount() here. csv_begin() already mounted the volume.
    // If someone calls this standalone, we can lazily retry on NOT_READY.

    FRESULT fr;
    f_mkdir("0:/pico_test"); // OK if exists

    fr = f_open(&g_bench_csv, BENCH_PATH, FA_OPEN_ALWAYS | FA_WRITE);
    if (fr == FR_NOT_READY) {
        // Fallback: allow standalone use (no csv_begin() before)
        fr = f_mount(&g_fs, "0:", 1);
        if (fr != FR_OK) { _friendly_mount_error(fr); return fr; }
        fr = f_open(&g_bench_csv, BENCH_PATH, FA_OPEN_ALWAYS | FA_WRITE);
    }
    if (fr != FR_OK) {
        printf("ERROR: Could not open %s (err=%d).\r\n", BENCH_PATH, fr);
        return fr;
    }

    if (f_size(&g_bench_csv) == 0) {
        const char *hdr =
            "timestamp_ms,jedec_hex,spi_hz,avg_erase_ms,avg_write256_kBps,avg_readseq_kBps,avg_readrand_MBps,verify_errors\r\n"
;
        UINT bw = 0;
        fr = f_write(&g_bench_csv, hdr, (UINT)strlen(hdr), &bw);
        if (fr != FR_OK || bw != (UINT)strlen(hdr)) {
            printf("ERROR: Failed writing benchmark header (err=%d).\r\n", fr);
        }
        f_sync(&g_bench_csv);
    }

    f_lseek(&g_bench_csv, f_size(&g_bench_csv)); // append
    g_bench_open = true;
    return FR_OK;
}

void bench_csv_append_avg(const char *jedec_hex,
                          uint32_t hz,
                          double avg_erase_ms,
                          double avg_write_kBps,
                          double avg_readseq_kBps,
                          double avg_readrand_MBps,
                          uint32_t verify_errors)

{
    if (!g_bench_open) return;
    char line[196];
    uint32_t t_ms = to_ms_since_boot(get_absolute_time());
    int n = snprintf(line, sizeof line,
    "%u,%s,%u,%.3f,%.3f,%.3f,%.3f,%u\r\n",
    t_ms,
    (jedec_hex && *jedec_hex) ? jedec_hex : "000000",
    hz, avg_erase_ms, avg_write_kBps, avg_readseq_kBps, avg_readrand_MBps, verify_errors);
    if (n > 0 && n < (int)sizeof line) {
        UINT bw=0; FRESULT fr = f_write(&g_bench_csv, line, (UINT)n, &bw);
        if (fr != FR_OK || bw != (UINT)n) printf("ERROR: benchmark.csv append err=%d\r\n", fr);
    }
}

void bench_csv_end(void) {
    if (!g_bench_open) return;
    f_sync(&g_bench_csv);
    f_close(&g_bench_csv);
    g_bench_open = false;
    // DO NOT f_unmount() here; csv_end() handles unmount for the session.
}


// Truncate CSV to a given byte position
FRESULT csv_truncate_to(DWORD pos) {
    // Open for R/W in case it's not already open
    if (!g_csv_open) {
        FRESULT fr = f_open(&g_csv, CSV_PATH, FA_OPEN_ALWAYS | FA_WRITE);
        if (fr != FR_OK) return fr;
        g_csv_open = true;
    }
    FRESULT fr = f_lseek(&g_csv, pos);
    if (fr == FR_OK) fr = f_truncate(&g_csv);
    f_sync(&g_csv);
    f_close(&g_csv);
    g_csv_open = false;
    return fr;
}

// Optional quick-undo for current session (without scanning)
void csv_undo_current_session(void) {
    if (g_last_session_offset == 0) {
        printf("No session to undo (this session didn't save yet).\r\n");
        return;
    }
    FRESULT fr = csv_truncate_to(g_last_session_offset);
    if (fr == FR_OK) printf("Undid last saved test.\r\n");
    else             printf("Undo failed (err=%d).\r\n", fr);
    g_last_session_offset = 0;
}
