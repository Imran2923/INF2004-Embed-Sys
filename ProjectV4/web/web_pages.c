#include "web_pages.h"
#include "web_output.h"
#include "net.h"
#include "http_server.h"
#include "ff.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#define SD_WEB_BASE "0:" 

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


/* ---------- tiny SD probe ---------- */
bool sd_ok(void) {
    FATFS fs;
    FRESULT fr = f_mount(&fs, "0:", 1);
    if (fr == FR_OK) { f_mount(0, "", 0); return true; }
    return false;
}

/* ---------- 200/404 headers ---------- */
static const char *HTTP200 =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html\r\n"
    "Connection: close\r\n\r\n";

static const char *HTTP404 =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: text/html\r\n"
    "Connection: close\r\n\r\n"
    "<html><body><h1>404 Not Found</h1></body></html>";

/* ---------- landing page ---------- */
void send_home_page(struct tcp_pcb *pcb) {
    http_write_str(pcb, HTTP200);

    bool ok = sd_ok();
    const char *sd_status = ok ? "Connected" : "Not Connected";
    const char *sd_class  = ok ? "info" : "error";

    char html[4096];
    snprintf(html, sizeof html,
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>Pico W SD Card Server</title>"
        "<style>"
        "body{font-family:system-ui,Arial;margin:40px;max-width:900px}"
        ".panel{background:#f5f5f7;border-radius:10px;padding:16px;margin-bottom:20px}"
        ".info{color:#2a8a3a;font-weight:700}.error{color:#c22;font-weight:700}"
        "a.btn{display:inline-block;margin-top:12px;padding:10px 14px;background:#0a6; color:#fff;border-radius:6px;text-decoration:none}"
        ".upload-box{border:2px dashed #ccc;padding:20px;border-radius:8px;text-align:center}"
        "input[type=file]{margin:10px 0}"
        "button{padding:10px 20px;background:#0a6;color:#fff;border:none;border-radius:6px;cursor:pointer;font-size:14px}"
        "button:hover{background:#088}"
        "</style></head><body>"
        "<h1>Pico W SD Card Server</h1>"
        "<div class='panel'>"
        "<p class='info'>Server is running!</p>"
        "<p class='%s'>SD Card: <b>%s</b></p>"
        "<p class='info'>WiFi: <b>Connected</b></p>"
        "<a class='btn' href='/sd?path=/'>Browse SD</a>"
        "<a class='btn' href='/menu'>Web Control Menu</a>"
        "</div>"
        "<div class='panel'>"
        "<h2>Upload File</h2>"
        "<div class='upload-box'>"
        "<form method='POST' action='/upload' enctype='multipart/form-data'>"
        "<input type='file' name='file' required><br>"
        "<button type='submit'>Upload to SD Card</button>"
        "</form>"
        "</div>"
        "</div>"
        "<p>Upload files directly to your SD card via WiFi!</p>"
        "</body></html>",
        sd_class, sd_status);

    http_write_str(pcb, html);
}

/* ---------- web menu page ---------- */
void send_web_menu(struct tcp_pcb *pcb) {
    http_write_str(pcb, HTTP200);
    
    char html[2048];
    snprintf(html, sizeof html,
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>SPI Flash Tool - Web Menu</title>"
        "<style>"
        "body{font-family:system-ui,Arial;margin:40px;max-width:900px}"
        ".panel{background:#f5f5f7;border-radius:10px;padding:16px;margin-bottom:20px}"
        ".menu-item{margin:10px 0;padding:12px;background:#fff;border-radius:6px;border:1px solid #ddd}"
        ".menu-item h3{margin:0 0 8px 0}"
        ".btn{display:inline-block;padding:8px 16px;background:#0a6;color:#fff;border-radius:4px;text-decoration:none;margin:4px}"
        ".btn:hover{background:#088}"
        ".btn-warning{background:#e90}"
        ".btn-warning:hover{background:#c70}"
        ".status{font-weight:bold;margin:5px 0}"
        ".online{color:#2a8a3a}"
        ".offline{color:#c22}"
        "</style></head><body>"
        "<h1>SPI Flash Tool - Web Interface</h1>"
        "<div class='panel'>"
        "<h2>System Status</h2>"
        "<div class='status'>WiFi: <span class='%s'>%s</span></div>"
        "<div class='status'>IP: %s</div>"
        "<div class='status'>HTTP Server: <span class='%s'>%s</span></div>"
        "</div>"
        "<div class='panel'>"
        "<h2>Benchmark Operations</h2>"
        "<div class='menu-item'>"
        "<h3>Quick Tests</h3>"
        "<a class='btn' href='/action?cmd=test_conn'>1. Test Connection</a>"
        "<a class='btn' href='/action?cmd=benchmark'>2. Run Benchmark</a>"
        "<a class='btn' href='/action?cmd=benchmark_100'>5. 100-run Demo</a>"
        "</div>"
        "<div class='menu-item'>"
        "<h3>Data Collection</h3>"
        "<a class='btn' href='/action?cmd=benchmark_save'>3. Benchmark + Save</a>"
        "<a class='btn' href='/action?cmd=read_results'>4. Read Results</a>"
        "<a class='btn btn-warning' href='/action?cmd=erase_last'>6. Erase Last Session</a>"
        "</div>"
        "<div class='menu-item'>"
        "<h3>Chip Analysis</h3>"
        "<a class='btn' href='/action?cmd=identify_chip'>7. Identify Chip</a>"
        "</div>"
        "<div class='menu-item'>"
        "<h3>File Management</h3>"
        "<a class='btn' href='/sd?path=/'>Browse SD Card</a>"
        "<a class='btn' href='/upload'>Upload Files</a>"
        "<a class='btn' href='/status'>8. System Status</a>"
        "</div>"
        "</div>"
        "<p><a href='/'>Back to Home</a></p>"
        "</body></html>",
        wifi_is_connected() ? "online" : "offline",
        wifi_is_connected() ? "Connected" : "Disconnected",
        wifi_get_ip_str(),
        http_server_is_running() ? "online" : "offline",
        http_server_is_running() ? "Running" : "Stopped"
    );
    
    http_write_str(pcb, html);
}

/* ---------- action result page ---------- */
void send_action_result_page(struct tcp_pcb *pcb, const char *cmd) {
    http_write_str(pcb, HTTP200);
    http_write_str(pcb,
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<title>Action Result</title>"
        "<style>"
        "body{font-family:system-ui,Arial;margin:40px}"
        "pre{background:#f5f5f7;padding:20px;border-radius:6px;white-space:pre-wrap;word-wrap:break-word}"
        "</style>"
        "</head><body>"
        "<h2>Action Result</h2>"
        "<pre>");
    
    // Send captured output to web (with basic HTML escaping)
    const char *output = get_web_output();
    const char *p = output;
    while (*p) {
        switch (*p) {
            case '<': http_write_str(pcb, "&lt;"); break;
            case '>': http_write_str(pcb, "&gt;"); break;
            case '&': http_write_str(pcb, "&amp;"); break;
            case '\n': http_write_str(pcb, "<br>"); break;
            case '\r': break; // skip
            default: 
                if (*p >= 32 && *p <= 126) {
                    char buf[2] = {*p, 0};
                    http_write_str(pcb, buf);
                } else {
                    http_write_str(pcb, ".");
                }
                break;
        }
        p++;
    }
    
    http_write_str(pcb, "</pre>");
    http_write_str(pcb, "<p><a href='/menu'>Back to Menu</a> | <a href='/'>Home</a></p>");
    http_write_str(pcb, "</body></html>");
}

/* ---------- directory listing ---------- */
void send_dir_listing(struct tcp_pcb *pcb, const char *path_qs) {
    char rel[256] = "/";
    if (path_qs && *path_qs) {
        strncpy(rel, path_qs, sizeof rel);
        rel[sizeof rel - 1] = 0;
        url_decode(rel);
    }
    /* sanitize */
    for (char *p = rel; *p; ++p) if (*p=='\\') *p='/';
    if (strstr(rel, "..")) strcpy(rel, "/");

    char abs[256];
    if (strcmp(rel, "/") == 0 || rel[0] == '\0') {
        snprintf(abs, sizeof abs, "%s", SD_WEB_BASE);
    } else {
        const char *rel_no_lead = (rel[0] == '/') ? rel + 1 : rel;
        snprintf(abs, sizeof abs, "%s/%s", SD_WEB_BASE, rel_no_lead);
    }
    printf("DEBUG: Opening directory: '%s'\n", abs);

    FATFS fs; FRESULT fr;
    fr = f_mount(&fs, "0:", 1);
    if (fr != FR_OK) { http_write_str(pcb, HTTP404); return; }

    DIR d; FILINFO fi;
    fr = f_opendir(&d, abs);
    if (fr != FR_OK) { f_mount(0,"",0); http_write_str(pcb, HTTP404); return; }

    http_write_str(pcb, HTTP200);
    http_write_str(pcb,
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<title>SD Browser</title><style>"
        "body{font:14px system-ui;margin:20px}table{border-collapse:collapse}"
        "td,th{padding:6px 10px;border-bottom:1px solid #ddd}"
        "a{text-decoration:none}"
        "</style></head><body>"
        "<h2>SD Browser</h2><p>Path: ");
    http_write_str(pcb, (*rel?rel:"/"));
    http_write_str(pcb, "</p>");

    /* Up link */
    if (*rel && strcmp(rel,"/")!=0) {
        char parent[256];
        strncpy(parent, rel, sizeof parent);
        parent[sizeof parent-1]=0;
        char *slash = strrchr(parent, '/');
        if (slash && slash!=parent) *slash = 0; else strcpy(parent, "/");
        http_write_str(pcb, "<p><a href='/sd?path=");
        http_write_str(pcb, parent);
        http_write_str(pcb, "'>&larr; Up</a></p>");
    }

    http_write_str(pcb, "<table><tr><th>Name</th><th>Size</th></tr>");

    char row[512];
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (!strcmp(fi.fname,".") || !strcmp(fi.fname,"..")) continue;

        char entry_rel[256];
        if (strcmp(rel, "/") == 0) {
            snprintf(entry_rel, sizeof entry_rel, "/%s", fi.fname);
        } else {
            snprintf(entry_rel, sizeof entry_rel, "%s/%s", rel, fi.fname);
        }

        if (fi.fattrib & AM_DIR) {
            snprintf(row, sizeof row,
                "<tr><td><a href='/sd?path=%s'>%s/</a></td><td>-</td></tr>",
                entry_rel, fi.fname);
        } else {
            snprintf(row, sizeof row,
                "<tr><td><a href='/get?path=%s'>%s</a></td><td>%lu</td></tr>",
                entry_rel, fi.fname, (unsigned long)fi.fsize);
        }
        http_write_str(pcb, row);
    }
    http_write_str(pcb, "</table><p><a href=\"/\">Home</a></p></body></html>");

    f_closedir(&d);
    f_mount(0, "", 0);
}

/* ---------- file download ---------- */
void send_file_download(struct tcp_pcb *pcb, const char *path_qs) {
    if (!path_qs || !*path_qs) { http_write_str(pcb, HTTP404); return; }

    char rel[256]; strncpy(rel, path_qs, sizeof rel); rel[sizeof rel-1]=0; url_decode(rel);
    for (char *p=rel; *p; ++p) if (*p=='\\') *p='/';
    if (strstr(rel, "..")) { http_write_str(pcb, HTTP404); return; }
    const char *fname = strrchr(rel, '/');
    fname = fname ? fname + 1 : rel;
    if (!*fname) fname = "download.bin";

    char abs[256];
    const char *rel_no_lead = (rel[0] == '/') ? rel + 1 : rel;
    snprintf(abs, sizeof abs, "%s/%s", SD_WEB_BASE, rel_no_lead);

    FATFS fs; FRESULT fr = f_mount(&fs, "0:", 1);
    if (fr != FR_OK) { http_write_str(pcb, HTTP404); return; }

    FIL f; fr = f_open(&f, abs, FA_READ);
    if (fr != FR_OK) { f_mount(0,"",0); http_write_str(pcb, HTTP404); return; }

    char hdr[256];
    snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Connection: close\r\n\r\n", fname);
    http_write_str(pcb, hdr);

    static uint8_t buf[1024];
    UINT br;
    do {
        br = 0;
        fr = f_read(&f, buf, sizeof buf, &br);
        if (fr != FR_OK) break;
        if (br) {
            err_t e = tcp_write(pcb, buf, br, 0);
            if (e != ERR_OK) break;
        }
    } while (br > 0);

    f_close(&f);
    f_mount(0, "", 0);
}

/* ---------- upload response ---------- */
/* ---------- upload response ---------- */
void send_upload_response(struct tcp_pcb *pcb, const char *filename, uint32_t bytes_received, bool success) {
    if (success) {
        http_write_str(pcb,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='3;url=/'></head><body>"
            "<h2>Upload Successful!</h2><p>File uploaded: <b>");
        http_write_str(pcb, filename);
        http_write_str(pcb, "</b> (");
        char size_buf[32]; 
        snprintf(size_buf, sizeof size_buf, "%lu", (unsigned long)bytes_received);
        http_write_str(pcb, size_buf);
        http_write_str(pcb, " bytes)</p><p><a href='/'>Home</a> | <a href='/sd?path=/'>Browse Files</a></p></body></html>");
    } else {
        http_write_str(pcb,
            "HTTP/1.1 500 Upload Failed\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n\r\n"
            "<!DOCTYPE html><html><body>"
            "<h2>Upload Failed!</h2><p>Could not save file to SD card</p>"
            "<p><a href='/'>Back to Home</a></p></body></html>");
    }
}