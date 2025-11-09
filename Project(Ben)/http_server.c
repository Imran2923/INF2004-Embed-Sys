// http_server.c - MODULAR VERSION
// Now uses web_pages.c, web_actions.c, and web_output.c

#include "http_server.h"
#include "web_pages.h"      // Add these includes
#include "web_actions.h"
#include "web_output.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "ff.h"
#include "sd_card.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

/* Expose SD to FatFs glue if you use diskio.c */
sd_card_t *g_sd_http = NULL;
static sd_card_t *g_sd_card = NULL;

static bool     s_http_running = false;
static uint16_t s_http_port    = HTTP_PORT;

/* ---------- small helpers ---------- */
static void http_write_str(struct tcp_pcb *pcb, const char *s) {
    if (!pcb || !s) return;
    tcp_write(pcb, s, strlen(s), TCP_WRITE_FLAG_COPY);
}

static char from_hex(char c){
    if (c>='0'&&c<='9') return c-'0';
    if (c>='A'&&c<='F') return c-'A'+10;
    if (c>='a'&&c<='f') return c-'a'+10;
    return 0;
}

static void url_decode(char *s){
    char *o=s;
    while (*s) {
        if (*s=='%' && s[1] && s[2]) { *o++ = (from_hex(s[1])<<4)|from_hex(s[2]); s+=3; }
        else if (*s=='+') { *o++=' '; ++s; }
        else { *o++=*s++; }
    }
    *o = 0;
}

/* set to "" to expose full card (root), or a subfolder like "0:/pico_test" */
#define SD_WEB_BASE  "0:"

/* ---------- TCP state ---------- */
typedef struct TCP_SERVER_T_ {
    struct tcp_pcb *server_pcb;
    struct tcp_pcb *client_pcb;
    bool uploading;
    FIL upload_file;
    char upload_path[256];
    char boundary[128];
    uint32_t content_length;
    uint32_t bytes_received;
    bool headers_done;
    FATFS upload_fs;
    char filename[128];
    bool headers_parsed;
    uint32_t last_activity;
} TCP_SERVER_T_;

static TCP_SERVER_T_ *s = NULL;

/* ---------- close client ---------- */
err_t tcp_server_close(void *arg) {
    TCP_SERVER_T_ *st = (TCP_SERVER_T_*)arg;
    if (st && st->client_pcb) {
        if (st->uploading) {
            f_close(&st->upload_file);
            f_mount(0, "", 0);
            st->uploading = false;
            printf("Upload connection closed\n");
        }

        tcp_arg(st->client_pcb, NULL);
        tcp_poll(st->client_pcb, NULL, 0);
        tcp_sent(st->client_pcb, NULL);
        tcp_recv(st->client_pcb, NULL);
        tcp_err(st->client_pcb, NULL);
        err_t e = tcp_close(st->client_pcb);
        if (e != ERR_OK) { tcp_abort(st->client_pcb); }
        st->client_pcb = NULL;
    }
    return ERR_OK;
}

/* ---------- extract filename from multipart ---------- */
static bool extract_filename(const char *data, size_t len, char *filename, size_t fname_size) {
    const char *content_disp = strstr(data, "Content-Disposition:");
    if (!content_disp || content_disp >= data + len) return false;

    const char *fn_start = strstr(content_disp, "filename=\"");
    if (!fn_start || fn_start >= data + len) return false;

    fn_start += 10;
    const char *fn_end = strchr(fn_start, '"');
    if (!fn_end || fn_end >= data + len) return false;

    const char *basename = fn_start;
    for (const char *p = fn_start; p < fn_end; p++) {
        if (*p == '/' || *p == '\\') basename = p + 1;
    }
    size_t fname_len = fn_end - basename;
    if (fname_len >= fname_size) fname_len = fname_size - 1;

    memcpy(filename, basename, fname_len);
    filename[fname_len] = '\0';

    printf("DEBUG: Extracted filename: '%s'\n", filename);
    return fname_len > 0;
}

/* ---------- extract boundary robustly ---------- */
static bool extract_boundary(const char *data, size_t len, char *boundary_out, size_t out_sz) {
    const char *ct = strstr(data, "Content-Type:");
    if (!ct || ct >= data + len) return false;
    const char *b = strstr(ct, "boundary=");
    if (!b || b >= data + len) return false;
    b += 9;

    const char *end = b;
    while (end < data + len && *end != '\r' && *end != '\n' && *end != ';' && !isspace((unsigned char)*end)) end++;
    size_t blen = end - b;
    if (blen + 3 >= out_sz) return false;

    boundary_out[0] = '-';
    boundary_out[1] = '-';
    memcpy(boundary_out + 2, b, blen);
    boundary_out[blen + 2] = '\0';
    return true;
}

/* ---------- find file data start ---------- */
static const char *find_file_data_start(const char *data, size_t len) {
    // First find the end of HTTP headers
    const char *http_headers_end = strstr(data, "\r\n\r\n");
    if (!http_headers_end) {
        return NULL; // Headers not complete yet
    }
    
    const char *search_start = http_headers_end + 4;
    size_t search_len = len - (search_start - data);
    
    if (search_len < 4) {
        return NULL; // Not enough data after headers
    }
    
    // Look for multipart headers end
    const char *multipart_headers_end = strstr(search_start, "\r\n\r\n");
    if (!multipart_headers_end) {
        return NULL; // Multipart headers not complete
    }
    
    return multipart_headers_end + 4;
}

/* ---------- reset upload state ---------- */
static void reset_upload_state(void) {
    if (!s) return;
    
    s->uploading = false;
    s->boundary[0] = '\0';
    s->filename[0] = '\0';
    s->bytes_received = 0;
    s->headers_parsed = false;
    s->last_activity = to_ms_since_boot(get_absolute_time());
}

/* ---------- safe SD card operations ---------- */
static bool safe_sd_mount(void) {
    for (int retry = 0; retry < 3; retry++) {
        FRESULT fr = f_mount(&s->upload_fs, "0:", 1);
        if (fr == FR_OK) return true;
        printf("SD mount failed (attempt %d): %d\n", retry + 1, fr);
        sleep_ms(10);
    }
    return false;
}

static bool safe_file_open(const char *path) {
    for (int retry = 0; retry < 3; retry++) {
        FRESULT fr = f_open(&s->upload_file, path, FA_CREATE_ALWAYS | FA_WRITE);
        if (fr == FR_OK) return true;
        printf("File open failed (attempt %d): %d, path: %s\n", retry + 1, fr, path);
        sleep_ms(10);
    }
    return false;
}

/* ---------- initialize upload ---------- */
static bool initialize_upload(struct tcp_pcb *pcb, const char *data, size_t len) {
    printf("Initializing upload...\n");
    
    // If we don't have filename from headers yet, try to extract it
    if (s->filename[0] == '\0') {
        char filename[64] = "upload.bin";
        if (!extract_filename(data, len, filename, sizeof(filename))) {
            printf("No filename found, using default\n");
        }
        strncpy(s->filename, filename, sizeof(s->filename) - 1);
    }
    
    // If we don't have boundary from headers yet, try to extract it  
    if (s->boundary[0] == '\0') {
        if (!extract_boundary(data, len, s->boundary, sizeof(s->boundary))) {
            printf("ERROR: No boundary found\n");
            return false;
        }
    }
    
    printf("Upload details - Filename: '%s', Boundary: '%s'\n", s->filename, s->boundary);
    
    // Mount SD card with retry
    if (!safe_sd_mount()) {
        printf("ERROR: SD mount failed after retries\n");
        return false;
    }
    
    // Create file with retry
    snprintf(s->upload_path, sizeof(s->upload_path), "%s/%s", SD_WEB_BASE, s->filename);
    printf("Creating file: %s\n", s->upload_path);
    
    if (!safe_file_open(s->upload_path)) {
        printf("ERROR: File create failed after retries\n");
        f_mount(0, "", 0);
        return false;
    }
    
    s->uploading = true;
    s->bytes_received = 0;
    s->last_activity = to_ms_since_boot(get_absolute_time());
    
    printf("Upload initialized successfully\n");
    return true;
}

/* ---------- process upload data ---------- */
static err_t process_upload_data(struct tcp_pcb *pcb, const char *data, size_t len) {
    if (!s->uploading) {
        return ERR_OK;
    }
    
    const char *boundary_pos = strstr(data, s->boundary);
    size_t write_len;
    
    if (boundary_pos) {
        // Found boundary - write data up to boundary
        const char *actual_end = boundary_pos;
        if (actual_end > data + 2 && actual_end[-2] == '\r' && actual_end[-1] == '\n') {
            actual_end -= 2;
        }
        write_len = actual_end - data;
    } else {
        // No boundary found - write all data
        write_len = len;
    }
    
    if (write_len > 0) {
        UINT bw;
        FRESULT fr = f_write(&s->upload_file, data, write_len, &bw);
        if (fr != FR_OK) {
            printf("Write failed: %d\n", fr);
            f_close(&s->upload_file);
            f_mount(0, "", 0);
            s->uploading = false;
            send_upload_response(pcb, s->filename, s->bytes_received, false); // From web_pages.c
            return ERR_ABRT;
        }
        s->bytes_received += bw;
        printf("Wrote %u bytes (total: %lu)\n", bw, (unsigned long)s->bytes_received);
    }
    
    if (boundary_pos) {
        // Upload complete
        f_close(&s->upload_file);
        f_mount(0, "", 0);
        s->uploading = false;
        printf("Upload completed: %s (%lu bytes)\n", s->filename, (unsigned long)s->bytes_received);
        send_upload_response(pcb, s->filename, s->bytes_received, true); // From web_pages.c
        return ERR_ABRT;
    }
    
    return ERR_OK;
}

/* ---------- handle file upload ---------- */
static err_t handle_upload(struct tcp_pcb *pcb, const char *data, size_t len) {
    printf("=== UPLOAD HANDLER: len=%u, uploading=%d, headers_parsed=%d ===\n", 
           (unsigned)len, s->uploading, s->headers_parsed);
    
    if (!s->uploading) {
        // Check if this is the start of an upload
        if (strstr(data, "POST /upload") == NULL) {
            printf("Not an upload request\n");
            return ERR_OK;
        }
        
        // If we haven't parsed headers yet, try to extract boundary and filename
        if (!s->headers_parsed) {
            printf("Parsing headers from incoming data...\n");
            
            // Extract boundary
            if (s->boundary[0] == '\0') {
                if (extract_boundary(data, len, s->boundary, sizeof(s->boundary))) {
                    printf("Boundary found: '%s'\n", s->boundary);
                } else {
                    printf("No boundary found yet, waiting for more data\n");
                    return ERR_OK; // Wait for more data
                }
            }
            
            // Extract filename
            if (s->filename[0] == '\0') {
                char filename[64] = "upload.bin";
                if (extract_filename(data, len, filename, sizeof(filename))) {
                    strncpy(s->filename, filename, sizeof(s->filename) - 1);
                    printf("Filename found: '%s'\n", s->filename);
                }
            }
            
            s->headers_parsed = true;
            printf("Headers parsed successfully\n");
        }
        
        // Look for file data start
        const char *file_start = find_file_data_start(data, len);
        if (file_start) {
            printf("File data found! Initializing upload...\n");
            if (!initialize_upload(pcb, data, len)) {
                return ERR_ABRT;
            }
            
            // Process the file data we found
            size_t data_offset = file_start - data;
            if (data_offset < len) {
                size_t file_data_len = len - data_offset;
                printf("Processing %u bytes of file data from first packet\n", (unsigned)file_data_len);
                return process_upload_data(pcb, file_start, file_data_len);
            }
        } else {
            printf("No file data in this packet, but headers are ready\n");
            // We have headers but no file data - initialize upload and wait for data packets
            if (s->boundary[0] != '\0' && !s->uploading) {
                printf("Initializing upload to wait for file data...\n");
                if (!initialize_upload(pcb, data, len)) {
                    return ERR_ABRT;
                }
                printf("Upload initialized, waiting for file data in next packet\n");
            }
        }
    } else {
        // We're already uploading - process this data as file content
        printf("Processing upload data: %u bytes\n", (unsigned)len);
        return process_upload_data(pcb, data, len);
    }
    
    return ERR_OK;
}

/* ---------- request parsing ---------- */
static const char *after_prefix(const char *s, const char *prefix) {
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n)==0 ? s+n : NULL;
}

static bool get_qs_value(const char *req, const char *key, char *out, size_t out_sz) {
    const char *line_end = strstr(req, "\r\n");
    if (!line_end) line_end = req + strlen(req);

    const char *p = strstr(req, key);
    if (!p || p > line_end) return false;
    p += strlen(key);

    size_t n = 0;
    while (p < line_end && *p != ' ' && *p != '&' && n + 1 < out_sz) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    return n > 0;
}

/* ---------- TCP callbacks ---------- */
static err_t tcp_server_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)err;
    TCP_SERVER_T_ *st = (TCP_SERVER_T_*)arg;

    if (!p) {
        if (st && st->uploading) {
            f_close(&st->upload_file);
            f_mount(0, "", 0);
            st->uploading = false;
            printf("Upload interrupted (connection closed)\n");
        }
        return tcp_server_close(arg);
    }

    // For uploads in progress, handle data directly without malloc
    if (st->uploading) {
        char *data = (char*)p->payload;
        size_t len = p->len;
        
        err_t result = process_upload_data(pcb, data, len);
        pbuf_free(p);
        
        if (result == ERR_ABRT) {
            tcp_output(pcb);
            return tcp_server_close(arg);
        }
        return ERR_OK;
    }

    // For regular requests, use malloc but limit size
    if (p->tot_len > 4096) {
        printf("Request too large: %d bytes\n", p->tot_len);
        pbuf_free(p);
        return tcp_server_close(arg);
    }

    char *req = malloc(p->tot_len + 1);
    if (!req) {
        pbuf_free(p);
        return tcp_server_close(arg);
    }

    pbuf_copy_partial(p, req, p->tot_len, 0);
    req[p->tot_len] = '\0';
    tcp_recved(pcb, p->tot_len);

    if (after_prefix(req, "POST /upload")) {
        err_t result = handle_upload(pcb, req, p->tot_len);
        pbuf_free(p);
        free(req);

        if (result == ERR_ABRT) {
            tcp_output(pcb);
            return tcp_server_close(arg);
        }
        return ERR_OK;
    }

    // === UPDATED ROUTING - Now uses modular functions ===
    if (after_prefix(req, "GET /sd")) {
        char path[256];
        if (!get_qs_value(req, "path=", path, sizeof path)) strcpy(path, "/");
        send_dir_listing(pcb, path);  // From web_pages.c

    } else if (after_prefix(req, "GET /get")) {
        char path[256];
        if (get_qs_value(req, "path=", path, sizeof path)) {
            send_file_download(pcb, path);  // From web_pages.c
        } else {
            http_write_str(pcb, "HTTP/1.1 404 Not Found\r\n\r\n");
        }

    } else if (after_prefix(req, "GET /menu")) {
        send_web_menu(pcb);  // From web_pages.c

    } else if (after_prefix(req, "GET /action")) {
    char cmd[64];
    if (get_qs_value(req, "cmd=", cmd, sizeof cmd)) {
        // Add small delay to prevent rapid duplicate requests
        sleep_ms(100);
        
        // Route to appropriate web action handler
        if (strcmp(cmd, "test_conn") == 0) {
            web_test_connection();
        } else if (strcmp(cmd, "benchmark") == 0) {
            web_run_benchmark();
        } else if (strcmp(cmd, "benchmark_save") == 0) {
            web_run_benchmark_save();
        } else if (strcmp(cmd, "read_results") == 0) {
            web_read_results();
        } else if (strcmp(cmd, "benchmark_100") == 0) {
            web_run_benchmark_100();
        } else if (strcmp(cmd, "erase_last") == 0) {
            web_erase_last_session();
        } else if (strcmp(cmd, "identify_chip") == 0) {
            web_identify_chip();
        } else if (strcmp(cmd, "backup_chip") == 0) {
            // Prevent duplicate backups within 2 minutes
            static uint32_t last_backup_time = 0;
            uint32_t current_time = to_ms_since_boot(get_absolute_time());
            
            if (last_backup_time != 0 && (current_time - last_backup_time < 120000)) {
                web_printf("Backup was recently run. Please wait 2 minutes.\r\n");
            } else {
                last_backup_time = current_time;
                web_backup_chip();
            }
        } else if (strcmp(cmd, "restore_chip") == 0) {
            // Prevent duplicate restores within 2 minutes
            static uint32_t last_restore_time = 0;
            uint32_t current_time = to_ms_since_boot(get_absolute_time());
            
            if (last_restore_time != 0 && (current_time - last_restore_time < 120000)) {
                web_printf("Restore was recently run. Please wait 2 minutes.\r\n");
            } else {
                last_restore_time = current_time;
                web_restore_chip();
            }
        } else {
            web_printf("Unknown command: %s", cmd);
        }
        send_action_result_page(pcb, cmd);
    } else {
        http_write_str(pcb, "HTTP/1.1 400 Bad Request\r\n\r\nMissing cmd parameter");
    }
    } else if (after_prefix(req, "GET /status")) {
    web_show_status();  // Show system status
    send_action_result_page(pcb, "status");
    } else {
        send_home_page(pcb);  // From web_pages.c
    }

    pbuf_free(p);
    free(req);
    tcp_output(pcb);
    return tcp_server_close(arg);
}

static err_t tcp_server_accept(void *arg, struct tcp_pcb *client_pcb, err_t err) {
    if (err != ERR_OK || !client_pcb) return ERR_VAL;
    if (!s) return ERR_VAL;

    printf("New client connected\n");

    // Clean up any previous state
    if (s->uploading) {
        printf("Cleaning up previous upload state\n");
        f_close(&s->upload_file);
        f_mount(0, "", 0);
    }
    
    reset_upload_state(); // Reset all upload state
    
    s->client_pcb = client_pcb;
    tcp_arg(client_pcb, s);
    tcp_recv(client_pcb, tcp_server_recv);
    
    return ERR_OK;
}

/* ---------- SD card diagnostics ---------- */
static void debug_sd_status(void) {
    FATFS fs;
    FRESULT fr = f_mount(&fs, "0:", 1);
    printf("=== SD CARD STATUS ===\n");
    printf("Mount result: %d (%s)\n", fr, fr == FR_OK ? "OK" : "FAILED");
    
    if (fr == FR_OK) {
        DWORD free_clusters, total_clusters;
        FATFS* fs_ptr;
        fr = f_getfree("0:", &free_clusters, &fs_ptr);
        if (fr == FR_OK) {
            total_clusters = fs_ptr->n_fatent - 2;
            printf("Free clusters: %lu/%lu\n", free_clusters, total_clusters);
            printf("Free space: ~%lu KB\n", (free_clusters * fs_ptr->csize * 512) / 1024);
        }
        
        // Test file creation
        FIL test_file;
        fr = f_open(&test_file, "0:/write_test.tmp", FA_CREATE_NEW | FA_WRITE);
        if (fr == FR_OK) {
            UINT bw;
            const char *test_data = "SD card write test";
            fr = f_write(&test_file, test_data, strlen(test_data), &bw);
            f_close(&test_file);
            if (fr == FR_OK) {
                printf("Write test: PASSED (%u bytes written)\n", bw);
                f_unlink("0:/write_test.tmp");
            } else {
                printf("Write test: FAILED (write error: %d)\n", fr);
            }
        } else {
            printf("Write test: FAILED (create error: %d)\n", fr);
        }
        
        f_mount(0, "", 0);
    }
    printf("=====================\n");
}

/* ---------- public API ---------- */
void http_server_init(sd_card_t *sd_card) {
    g_sd_card = sd_card;
    g_sd_http = sd_card;

    // Run SD card diagnostics
    debug_sd_status();

    if (!s) {
        s = calloc(1, sizeof *s);
        if (!s) { printf("HTTP: no mem\n"); return; }
    }

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) { printf("HTTP: tcp_new_ip_type failed\n"); return; }

    if (tcp_bind(pcb, IP_ANY_TYPE, HTTP_PORT) != ERR_OK) {
        printf("HTTP: bind failed\n"); tcp_close(pcb); return;
    }
    s->server_pcb = tcp_listen_with_backlog(pcb, 1);
    if (!s->server_pcb) { printf("HTTP: listen failed\n"); tcp_close(pcb); return; }

    tcp_arg(s->server_pcb, s);
    tcp_accept(s->server_pcb, tcp_server_accept);

    s_http_running = true;
    printf("HTTP server started on port %u\n", HTTP_PORT);
    printf("Uploads will be saved to: %s/\n", SD_WEB_BASE);
}

bool     http_server_is_running(void){ return s_http_running; }
uint16_t http_server_port(void)       { return s_http_port;   }

void http_server_run(void) { }