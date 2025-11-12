#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include "pico/stdlib.h"
#include "ff.h"
#include "config.h"
#include "flash.h"   // for read_jedec_id()


typedef struct {
    char  model[48];
    char  company[64];
    char  family[64];
    double cap_mbit;
    char  jedec[24];
    uint8_t  jedec_mfg;   // NEW: parsed mfg byte
    uint16_t jedec_dev;   // NEW: parsed 2-byte device code (hi:lo)
    double typ_erase_ms;
    double max_erase_ms;
    double typ_erase32_ms;
    double max_erase32_ms;
    double typ_erase64_ms;
    double max_erase64_ms;
    double max_read_mhz;
    double typ_prog_ms;
    double max_prog_ms;
    double read_50_mb_s;
    char  v_range[24];
    double endurance;
} chip_ref_t;


// Files we read
#define BENCH_PATH "0:/pico_test/benchmark.csv"
#define REF_PATH   "0:/pico_test/spichips.csv"

// Local FatFs objects (do NOT use csvlog.c internals)
static FATFS g_fs_ana;

// --- tiny helpers ---

static inline double clamp01(double x){ return x < 0 ? 0 : (x > 1 ? 1 : x); }

// Remove UTF-8 BOM if present
static void strip_bom(char *s) {
    unsigned char *u = (unsigned char*)s;
    if (u[0]==0xEF && u[1]==0xBB && u[2]==0xBF) {
        memmove(s, s+3, strlen(s+3)+1);
    }
}

static int find_col_multi(char **hdr, int n, const char *alts[], int nalts) {
    for (int a = 0; a < nalts; ++a) {
        for (int i = 0; i < n; ++i) {
            if (!strcasecmp(hdr[i], alts[a])) return i;
        }
    }
    return -1;
}

// Trim spaces and trailing CR/LF
static void trim_spaces_crlf(char *s) {
    // strip CR/LF
    size_t n = strlen(s);
    while (n && (s[n-1]=='\r' || s[n-1]=='\n')) s[--n] = 0;
    // left trim
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p)+1);
    // right trim
    n = strlen(s);
    while (n && isspace((unsigned char)s[n-1])) s[--n] = 0;
}

// Remove all double quotes (we don’t need them)
static void remove_all_quotes(char *s) {
    char *w = s;
    for (char *r=s; *r; ++r) if (*r != '"') *w++ = *r;
    *w = 0;
}

static void rstrip_crlf(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1] == '\r' || s[n-1] == '\n' || isspace((unsigned char)s[n-1]))) {
        s[--n] = 0;
    }
}
static void strip_quotes(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n-1] == '"') {
        // remove surrounding quotes in-place
        memmove(s, s+1, n-2);
        s[n-2] = 0;
    }
}

static void trim_spaces(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p)+1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n-1])) s[--n] = 0;
}

static void strip_crlf(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1]=='\r' || s[n-1]=='\n')) s[--n] = 0;
}

static void strip_quotes_if_any(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && s[0]=='"' && s[n-1]=='"') {
        memmove(s, s+1, n-2);
        s[n-2] = 0;
    }
}

static char* next_tok(char **ps) {
    char *tok = strtok(*ps, ",");
    *ps = NULL;                  // subsequent calls use NULL
    if (!tok) return NULL;
    rstrip_crlf(tok);
    // trim leading/trailing spaces
    while (*tok && isspace((unsigned char)*tok)) tok++;
    char *end = tok + strlen(tok);
    while (end > tok && isspace((unsigned char)end[-1])) *--end = 0;
    strip_quotes(tok);
    return tok;
}

// Convert our write throughput (kB/s for 256B) to an *effective* program time (ms) for 256B.
static inline double write_kBps_to_prog_ms(double write_kBps) {
    if (write_kBps <= 0) return 1e9;
    double bytes_per_s = write_kBps * 1024.0;
    double t_s = 256.0 / bytes_per_s;  // 256 bytes
    return t_s * 1000.0;               // ms
}

// Split by commas, keeping empty fields (",,") and handling quotes & BOM
static int csv_split_simple_keep_empty(char *line, char *cols[], int max_cols) {
    strip_bom(line);
    remove_all_quotes(line);
    trim_spaces_crlf(line);

    int count = 0;
    int in_quotes = 0;
    char *start = line;

    if (max_cols > 0) cols[count++] = start;

    for (char *c = line; *c; ++c) {
        if (*c == '"') {
            in_quotes = !in_quotes;
        } else if (*c == ',' && !in_quotes) {
            *c = '\0';
            if (count < max_cols) cols[count++] = c + 1;
        }
    }

    // Final trim per field
    for (int i = 0; i < count; ++i) trim_spaces_crlf(cols[i]);
    return count;
}

// safe strtod for optional numeric fields (empty -> 0.0)
static double to_dflt0(const char *s) { return (s && *s) ? strtod(s, NULL) : 0.0; }

// Read the *last* 12 MHz row from benchmark.csv into outputs.
// Returns true if found.
// Read the *last* 12 MHz row from benchmark.csv into outputs.
// Works with either header order:
//   A) jedec_hex,spi_hz,avg_erase_ms,...,verify_errors
//   B) timestamp_ms,jedec_hex,spi_hz,avg_erase_ms,...,verify_errors
static bool load_bench_12mhz(double *avg_erase_ms, double *avg_write_kBps,
                             double *avg_readseq_kBps, uint32_t *verify_errors)
{
    FRESULT fr = f_mount(&g_fs_ana, "0:", 1);
    if (fr != FR_OK) { printf("ERROR: mount err=%d\r\n", fr); return false; }

    FIL f; fr = f_open(&f, BENCH_PATH, FA_READ);
    if (fr != FR_OK) { printf("ERROR: open %s err=%d\r\n", BENCH_PATH, fr); f_unmount("0:"); return false; }

    char line[256];

    // --- read and parse header ---
    if (!f_gets(line, sizeof line, &f)) { f_close(&f); f_unmount("0:"); return false; }
    char *hdr[16] = {0};
    int nh = csv_split_simple_keep_empty(line, hdr, 16);
    if (nh <= 0) { f_close(&f); f_unmount("0:"); return false; }

    // locate the columns we need (support a few alternative names)
    const char *ALT_HZ[]    = {"spi_hz", "hz"};
    const char *ALT_ERASE[] = {"avg_erase_ms", "erase_ms"};
    const char *ALT_WK[]    = {"avg_write256_kBps", "avg_write_kBps"};
    const char *ALT_RK[]    = {"avg_readseq_kBps", "avg_read_kBps"};
    const char *ALT_VER[]   = {"verify_errors", "total_verify_errors", "total_verify_errs"};

    int i_hz    = find_col_multi(hdr, nh, ALT_HZ,    (int)(sizeof ALT_HZ   /sizeof ALT_HZ[0]));
    int i_erase = find_col_multi(hdr, nh, ALT_ERASE, (int)(sizeof ALT_ERASE/sizeof ALT_ERASE[0]));
    int i_wk    = find_col_multi(hdr, nh, ALT_WK,    (int)(sizeof ALT_WK   /sizeof ALT_WK[0]));
    int i_rk    = find_col_multi(hdr, nh, ALT_RK,    (int)(sizeof ALT_RK   /sizeof ALT_RK[0]));
    int i_ver   = find_col_multi(hdr, nh, ALT_VER,   (int)(sizeof ALT_VER  /sizeof ALT_VER[0]));

    if (i_hz < 0 || i_erase < 0 || i_wk < 0 || i_rk < 0 || i_ver < 0) {
        printf("ERROR: benchmark.csv header missing required columns.\r\n");
        f_close(&f); f_unmount("0:"); 
        return false;
    }

    bool found = false;

    // --- read data rows ---
    while (f_gets(line, sizeof line, &f)) {
        char *col[16] = {0};
        int n = csv_split_simple_keep_empty(line, col, 16);
        if (n <= i_ver) continue; // not enough columns

        uint32_t hz   = (uint32_t)strtoul(col[i_hz], NULL, 10);
        double   e_ms = to_dflt0(col[i_erase]);
        double   w_kB = to_dflt0(col[i_wk]);
        double   r_kB = to_dflt0(col[i_rk]);
        uint32_t verr = (uint32_t)strtoul(col[i_ver], NULL, 10);

        if (hz == 12000000u) {
            // keep the *last* 12 MHz row if there are multiple
            found = true;
            *avg_erase_ms     = e_ms;
            *avg_write_kBps   = w_kB;
            *avg_readseq_kBps = r_kB;
            *verify_errors    = verr;
        }
    }

    f_close(&f);
    f_unmount("0:");
    return found;
}

static bool parse_jedec_bytes(const char *s, uint8_t *mfg, uint16_t *dev){
    // accepts "JEDEC=9D:4013", "JEDEC-9D 4013", "9D 40 13" etc
    unsigned mi=0, d=0;
    // strip common prefixes
    while (*s==' ' || *s=='J' || *s=='E' || *s=='D' || *s=='C' || *s=='=' || *s=='-' ) s++;
    // try patterns
    if (sscanf(s, "%x:%x", &mi, &d) == 2) { *mfg=(uint8_t)mi; *dev=(uint16_t)d; return true; }
    unsigned b0=0,b1=0,b2=0;
    if (sscanf(s, "%x %x %x", &b0, &b1, &b2) == 3) { *mfg=(uint8_t)b0; *dev=(uint16_t)((b1<<8)|b2); return true; }
    return false;
}

typedef struct {
    char  model[64], company[64], family[64];
    double cap_mbit;
    char  jedec_raw[64];
    uint8_t jedec_mfg; uint16_t jedec_dev;
    double typ_erase_ms, max_erase_ms;
    double typ_prog_ms,  max_prog_ms;
    double read50_mb_s;
} ref_row_t;

static void get_live_jedec(uint8_t id[3]){
    // call your existing flash helper
    read_jedec_id(id); // returns 3 bytes: mfg, devHi, devLo
}

static double score_ref(const ref_row_t *r,
                        double erase_ms, double prog_ms,
                        double read_seq_MBps_12MHz, // from averages at 12 MHz
                        uint8_t live_mfg, uint16_t live_dev,
                        double cap_meas_mbit,
                        bool write_unreliable)
{
    // scale read to 50 MHz estimate if you only have 12 MHz
    double read50_est = read_seq_MBps_12MHz * (50.0/12.0);

    double score = 0.0;

    // JEDEC: hard gate
    if (live_mfg && live_dev) {
        if (r->jedec_mfg != 0 || r->jedec_dev != 0) {
            if (!(r->jedec_mfg == live_mfg && r->jedec_dev == live_dev)) score += 100.0;
        }
    }

    // Capacity penalty (if you know your tested part size)
    if (cap_meas_mbit > 0 && r->cap_mbit > 0) {
        score += 2.0 * fabs(cap_meas_mbit - r->cap_mbit);
    }

    // Relative timing errors (use small weights)
    if (r->typ_erase_ms > 0) {
        double re = (erase_ms - r->typ_erase_ms) / r->typ_erase_ms;
        score += 5.0 * re * re;
    }
    if (!write_unreliable && r->typ_prog_ms > 0) {
        double rp = (prog_ms - r->typ_prog_ms) / r->typ_prog_ms;
        score += 3.0 * rp * rp;
    }
    if (r->read50_mb_s > 0) {
        double rr = (read50_est - r->read50_mb_s) / r->read50_mb_s;
        score += 4.0 * rr * rr;
    }
    return score;
}


static int parse_ref_line(const char *line_in, chip_ref_t *out) {
    // Expected columns (17 total; last two optional):
    // chip_model,company,chip_family,capacity_mbit,jedec_id,
    // typ_4kb_sector_erase_ms,max_4kb_sector_erase_ms,
    // typ_32kb_block_erase_ms,max_32kb_block_erase_ms,
    // typ_64kb_block_erase_ms,max_64kb_block_erase_ms,
    // max_clock_read_mhz,typ_page_program_ms,max_page_program_ms,
    // read_speed_50mhz_mb_s,operating_voltage_range,endurance_cycles
    char buf[384];
    strncpy(buf, line_in, sizeof buf);
    buf[sizeof buf - 1] = 0;

    char *col[20] = {0};
    int n = csv_split_simple_keep_empty(buf, col, 20);
    if (n < 15) {
        // Uncomment to debug:
        // printf("Skip row (cols=%d): %s\r\n", n, line_in);
        return 0;
    }

    strncpy(out->model,   col[0], sizeof out->model);   out->model[sizeof out->model-1]=0;
    strncpy(out->company, col[1], sizeof out->company); out->company[sizeof out->company-1]=0;
    strncpy(out->family,  col[2], sizeof out->family);  out->family[sizeof out->family-1]=0;

    out->cap_mbit       = to_dflt0(col[3]);
    strncpy(out->jedec,  col[4], sizeof out->jedec);    out->jedec[sizeof out->jedec-1]=0;
    // NEW: parse JEDEC text (accepts formats like "9D 40 13" or "JEDEC=9D:4013")
    out->jedec_mfg = 0;
    out->jedec_dev = 0;
    (void)parse_jedec_bytes(out->jedec, &out->jedec_mfg, &out->jedec_dev);

    out->typ_erase_ms   = to_dflt0(col[5]);
    out->max_erase_ms   = to_dflt0(col[6]);
    out->typ_erase32_ms = to_dflt0(col[7]);
    out->max_erase32_ms = to_dflt0(col[8]);
    out->typ_erase64_ms = to_dflt0(col[9]);
    out->max_erase64_ms = to_dflt0(col[10]);
    out->max_read_mhz   = to_dflt0(col[11]);
    out->typ_prog_ms    = to_dflt0(col[12]);
    out->max_prog_ms    = to_dflt0(col[13]);
    out->read_50_mb_s   = to_dflt0(col[14]);

    if (n > 15) { strncpy(out->v_range, col[15], sizeof out->v_range); out->v_range[sizeof out->v_range-1]=0; }
    else out->v_range[0] = 0;
    out->endurance = (n > 16) ? to_dflt0(col[16]) : 0.0;

    return 1;
}

void identify_chip_from_bench_12mhz(void)
{
    double e_ms=0, w_kBps=0, rseq_kBps=0;
    uint32_t verr=0;
    if (!load_bench_12mhz(&e_ms, &w_kBps, &rseq_kBps, &verr)) {
        printf("No 12MHz averages found in %s.\r\n", BENCH_PATH);
        return;
    }
    if (verr) printf("NOTE: verify_errors=%u in averages; write metric may be unreliable.\r\n", verr);

    // Read live JEDEC from the device
    uint8_t live_id[3] = {0};
    read_jedec_id(live_id);           // from flash.h
    uint8_t  live_mfg = live_id[0];
    uint16_t live_dev = (uint16_t)((live_id[1] << 8) | live_id[2]);
    bool write_unreliable = (verr != 0);

    // Normalize to the fields in the reference:
    double prog_ms_meas = write_kBps_to_prog_ms(w_kBps);
    double read50_mb_s_meas = (rseq_kBps / 1024.0) * (50.0 / 12.0); // scale roughly with clock

    // Open reference
    FRESULT fr = f_mount(&g_fs_ana, "0:", 1);
    if (fr != FR_OK) { printf("ERROR: mount err=%d\r\n", fr); return; }

    FIL f; fr = f_open(&f, REF_PATH, FA_READ);
    if (fr != FR_OK) { printf("ERROR: open %s err=%d\r\n", REF_PATH, fr); f_unmount("0:"); return; }

    char line[512];
    // Skip header
    if (!f_gets(line, sizeof line, &f)) { f_close(&f); f_unmount("0:"); return; }

    // Scoring weights (tuneable)
    const double w_erase = 1.0;
    const double w_prog  = 1.0;
    const double w_read  = 0.7;

    typedef struct { chip_ref_t ref; double score; } hit_t;
    hit_t best[3]; for (int i=0;i<3;i++){ best[i].score = 1e99; memset(&best[i].ref,0,sizeof(best[i].ref)); }

    int accepted = 0;

    while (f_gets(line, sizeof line, &f)) {

        // skip empty lines
        if (line[0] == 0 || line[0] == '\r' || line[0] == '\n') continue;

        chip_ref_t r;
        if (!parse_ref_line(line, &r)) continue;
        accepted++;

        // Distance components (use relative % error, bounded)
        double d_erase = fabs(e_ms - r.typ_erase_ms) / fmax(1.0, r.typ_erase_ms);
        double d_prog  = fabs(prog_ms_meas - r.typ_prog_ms) / fmax(0.1, r.typ_prog_ms);
        double d_read  = (r.read_50_mb_s > 0.01)
            ? fabs(read50_mb_s_meas - r.read_50_mb_s) / r.read_50_mb_s
            : 0.0;

        // Weighted L1 distance
        double score = w_erase*d_erase + w_prog*d_prog + w_read*d_read;

        // JEDEC influence: heavy penalty if a row has JEDEC and it doesn't match;
        // small bonus if it does match. If the row has no JEDEC, leave score as-is.
        if (live_mfg || live_dev) {
            if (r.jedec_mfg || r.jedec_dev) {
                if (r.jedec_mfg == live_mfg && r.jedec_dev == live_dev) {
                    score -= 0.25;  // bonus for exact match (clamped by min later if needed)
                } else {
                    score += 100.0; // large penalty for mismatch
                }
            }
        }

        // If write was unreliable, down-weight program contribution by blending it away:
        if (write_unreliable) {
            // remove most of the prog error influence (already counted in score); keep a tiny hint
            score -= w_prog*d_prog;            // remove the prog term we added
            score += 0.15 * d_prog;            // add back a very small fraction to avoid zeroing it
        }

        // Maintain Top-3 lowest scores
        for (int i=0;i<3;i++){
            if (score < best[i].score) {
                for (int j=2;j>i;j--) best[j] = best[j-1];
                best[i].score = score; best[i].ref = r; break;
            }
        }
    }

    f_close(&f);
    f_unmount("0:");

    printf("\r\n=== Chip Identification (12 MHz) ===\r\n");
    printf("Measured: erase=%.2f ms, prog256=%.3f ms, read50~=%.2f MB/s\r\n",
           e_ms, prog_ms_meas, read50_mb_s_meas);
    printf("Reference rows accepted: %d\r\n", accepted);
    printf("Top matches:\r\n");
    for (int i=0;i<3;i++){
        if (best[i].score >= 1e99) continue;
        printf("%d) %s  [%s, %s]  JEDEC=%s  score=%.3f\r\n",
               i+1, best[i].ref.model, best[i].ref.company, best[i].ref.family,
               best[i].ref.jedec, best[i].score);
    }
    printf("(Lower score = closer match)\r\n");
}
