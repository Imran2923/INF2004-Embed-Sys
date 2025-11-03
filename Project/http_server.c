#include "http_server.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "ff.h"
#include "sd_card.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Expose SD to FatFs glue if you use diskio.c */
sd_card_t *g_sd_http = NULL;
static sd_card_t *g_sd_card = NULL;

static bool     s_http_running = false;
static uint16_t s_http_port    = HTTP_PORT;

/* ---------- small helpers ---------- */
static void http_write_str(struct tcp_pcb *pcb, const char *s) {
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
#define SD_WEB_BASE  "0:/pico_test"

/* ---------- tiny SD probe for landing page ---------- */
static bool sd_ok(void) {
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

/* ---------- TCP state ---------- */
typedef struct TCP_SERVER_T_ {
    struct tcp_pcb *server_pcb;
    struct tcp_pcb *client_pcb;
} TCP_SERVER_T;

static TCP_SERVER_T *s = NULL;

/* ---------- close client ---------- */
err_t tcp_server_close(void *arg) {
    TCP_SERVER_T *st = (TCP_SERVER_T*)arg;
    if (st && st->client_pcb) {
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

/* ---------- landing page ---------- */
static void send_home(struct tcp_pcb *pcb) {
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
        ".panel{background:#f5f5f7;border-radius:10px;padding:16px}"
        ".info{color:#2a8a3a;font-weight:700}.error{color:#c22;font-weight:700}"
        "a.btn{display:inline-block;margin-top:12px;padding:10px 14px;background:#0a6; color:#fff;border-radius:6px;text-decoration:none}"
        "</style></head><body>"
        "<h1>Pico W SD Card Server</h1>"
        "<div class='panel'>"
        "<p class='info'>Server is running!</p>"
        "<p class='%s'>SD Card: <b>%s</b></p>"
        "<p class='info'>WiFi: <b>Connected</b></p>"
        "<a class='btn' href='/sd?path=/'>Browse SD</a>"
        "</div>"
        "<p>This page is served by your Pico W. Click <b>Browse SD</b> to see files.</p>"
        "</body></html>",
        sd_class, sd_status);

    http_write_str(pcb, html);
}

/* ---------- directory listing ---------- */
static void send_dir_listing(struct tcp_pcb *pcb, const char *path_qs) {
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
    snprintf(abs, sizeof abs, "%s%s", SD_WEB_BASE, (rel[0]=='/') ? rel+1 : rel);

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
        char parent[256]; strncpy(parent, rel, sizeof parent); parent[sizeof parent-1]=0;
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
        if (fi.fattrib & AM_DIR) {
            snprintf(row, sizeof row,
                "<tr><td><a href='/sd?path=%s%s%s'>%s/</a></td><td>-</td></tr>",
                (*rel && strcmp(rel,"/"))? rel:"",
                (*rel && rel[strlen(rel)-1]=='/')? "":"/",
                fi.fname, fi.fname);
        } else {
            const char *base = (strcmp(rel, "/") == 0) ? "/" : rel;
            snprintf(row, sizeof row,
                "<tr><td><a href='/get?path=%s%s%s'>%s</a></td><td>%lu</td></tr>",
                base,
                (base[strlen(base)-1] == '/') ? "" : "/",
                fi.fname, fi.fname, (unsigned long)fi.fsize);
        }
        http_write_str(pcb, row);
    }
    http_write_str(pcb, "</table><p><a href=\"/\">Home</a></p></body></html>");

    f_closedir(&d);
    f_mount(0, "", 0);
}

/* ---------- file download ---------- */
static void send_file_download(struct tcp_pcb *pcb, const char *path_qs) {
    if (!path_qs || !*path_qs) { http_write_str(pcb, HTTP404); return; }

    char rel[256]; strncpy(rel, path_qs, sizeof rel); rel[sizeof rel-1]=0; url_decode(rel);
    for (char *p=rel; *p; ++p) if (*p=='\\') *p='/';
    if (strstr(rel, "..")) { http_write_str(pcb, HTTP404); return; }
    // derive a nice filename for Content-Disposition
    const char *fname = strrchr(rel, '/');
    fname = fname ? fname + 1 : rel;  // basename without folders
    if (!*fname) fname = "download.bin"; // fallback


    char abs[256]; snprintf(abs, sizeof abs, "%s/%s", SD_WEB_BASE, (rel[0]=='/') ? rel+1 : rel);

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
            /* send a chunk; lwIP buffers it for us */
            err_t e = tcp_write(pcb, buf, br, 0);
            if (e != ERR_OK) break;
        }
    } while (br > 0);

    f_close(&f);
    f_mount(0, "", 0);
}

/* ---------- request parsing ---------- */
static const char *after_prefix(const char *s, const char *prefix) {
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n)==0 ? s+n : NULL;
}

/* Replace the old get_qs with a safe copy variant */
static bool get_qs_value(const char *req, const char *key, char *out, size_t out_sz) {
    const char *line_end = strstr(req, "\r\n");
    if (!line_end) line_end = req + strlen(req);

    const char *p = strstr(req, key);
    if (!p || p > line_end) return false;       // key not in request path
    p += strlen(key);

    // copy until space (end of path) or & (next param)
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
    if (!p) return tcp_server_close(arg);

    char req[256];
    u16_t n = (p->tot_len < (u16_t)(sizeof req - 1)) ? p->tot_len : (sizeof req - 1);
    pbuf_copy_partial(p, req, n, 0);
    req[n] = 0;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    if (after_prefix(req, "GET /sd")) {
        char path[256];
        if (!get_qs_value(req, "path=", path, sizeof path)) strcpy(path, "/");
        send_dir_listing(pcb, path);

    } else if (after_prefix(req, "GET /get")) {
        char path[256];
        if (get_qs_value(req, "path=", path, sizeof path)) {
            send_file_download(pcb, path);
        } else {
            http_write_str(pcb, HTTP404);
        }

    } else {
        send_home(pcb);
    }

    tcp_output(pcb);
    return tcp_server_close(arg);
}


static err_t tcp_server_accept(void *arg, struct tcp_pcb *client_pcb, err_t err) {
    if (err != ERR_OK || !client_pcb) return ERR_VAL;
    s->client_pcb = client_pcb;
    tcp_arg(client_pcb, s);
    tcp_recv(client_pcb, tcp_server_recv);
    return ERR_OK;
}

/* ---------- public API ---------- */
void http_server_init(sd_card_t *sd_card) {
    g_sd_card = sd_card;
    g_sd_http = sd_card;

    s = calloc(1, sizeof *s);
    if (!s) { printf("HTTP: no mem\n"); return; }

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
}

bool     http_server_is_running(void){ return s_http_running; }
uint16_t http_server_port(void)       { return s_http_port;   }

/* nothing required here—lwIP runs by callbacks */
void http_server_run(void) { }
