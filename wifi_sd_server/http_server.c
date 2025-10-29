#include "http_server.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include <string.h>
#include <stdio.h>

static sd_card_t *g_sd_card = NULL;

// HTTP response headers
static const char *http_200_header = 
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html\r\n"
    "Connection: close\r\n\r\n";

static const char *http_404_header = 
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: text/html\r\n"
    "Connection: close\r\n\r\n"
    "<html><body><h1>404 Not Found</h1></body></html>";

// TCP connection state
typedef struct TCP_SERVER_T_ {
    struct tcp_pcb *server_pcb;
    struct tcp_pcb *client_pcb;
    uint8_t buffer[2048];
    int buffer_len;
} TCP_SERVER_T;

static TCP_SERVER_T *tcp_server_state = NULL;

// Close connection
static err_t tcp_server_close(void *arg) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    err_t err = ERR_OK;
    
    if (state->client_pcb != NULL) {
        tcp_arg(state->client_pcb, NULL);
        tcp_poll(state->client_pcb, NULL, 0);
        tcp_sent(state->client_pcb, NULL);
        tcp_recv(state->client_pcb, NULL);
        tcp_err(state->client_pcb, NULL);
        err = tcp_close(state->client_pcb);
        if (err != ERR_OK) {
            tcp_abort(state->client_pcb);
            err = ERR_ABRT;
        }
        state->client_pcb = NULL;
    }
    
    return err;
}

// Send HTTP response with DYNAMIC SD card status
static err_t tcp_server_send_data(void *arg, struct tcp_pcb *tpcb) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    
    // Send HTTP header
    err_t err = tcp_write(tpcb, http_200_header, strlen(http_200_header), TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        printf("Failed to write header\n");
        return err;
    }
    
    // Build dynamic HTML with REAL SD card status
    char html_buffer[4096];
    const char *sd_status = (g_sd_card && g_sd_card->initialized) ? "Connected" : "Not Connected";
    const char *sd_color = (g_sd_card && g_sd_card->initialized) ? "info" : "error";
    
    snprintf(html_buffer, sizeof(html_buffer),
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "<title>Pico W SD Card Server</title>"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<style>"
        "body { font-family: Arial, sans-serif; max-width: 800px; margin: 50px auto; padding: 20px; "
        "background: linear-gradient(135deg, #667eea 0%%, #764ba2 100%%); }"
        ".container { background: white; border-radius: 10px; padding: 30px; box-shadow: 0 10px 30px rgba(0,0,0,0.2); }"
        "h1 { color: #333; margin-top: 0; }"
        ".status { background: #f0f0f0; padding: 15px; border-radius: 5px; margin: 20px 0; }"
        ".info { color: #28a745; font-weight: bold; }"
        ".error { color: #dc3545; font-weight: bold; }"
        "button { background: #007bff; color: white; border: none; padding: 10px 20px; "
        "border-radius: 5px; cursor: pointer; font-size: 16px; }"
        "button:hover { background: #0056b3; }"
        "</style>"
        "</head>"
        "<body>"
        "<div class=\"container\">"
        "<h1>Pico W SD Card Server</h1>"
        "<div class=\"status\">"
        "<p class=\"info\">Server is running!</p>"
        "<p class=\"%s\">SD Card: <strong>%s</strong></p>"
        "<p class=\"info\">WiFi: <strong>Connected </strong></p>"
        "</div>"
        "<p>This is a basic C implementation of the SD card WiFi server.</p>"
        "<p>The server is currently serving this page. File browsing functionality "
        "requires implementing a FAT filesystem parser.</p>"
        "<h3>Next Steps:</h3>"
        "<ul>"
        "<li>Add FatFs library for proper file system support</li>"
        "<li>Implement directory listing</li>"
        "<li>Add file download capability</li>"
        "<li>Enhance UI with file browser</li>"
        "</ul>"
        "</div>"
        "</body>"
        "</html>",
        sd_color, sd_status);
    
    // Send HTML content
    err = tcp_write(tpcb, html_buffer, strlen(html_buffer), TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        printf("Failed to write data\n");
        return err;
    }
    
    // Flush data
    err = tcp_output(tpcb);
    if (err != ERR_OK) {
        printf("Failed to output data\n");
        return err;
    }
    
    return ERR_OK;
}

// Handle received data
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    
    if (!p) {
        printf("Connection closed by client\n");
        return tcp_server_close(arg);
    }
    
    // This example accepts any request and returns the same HTML
    if (p->tot_len > 0) {
        printf("Received %d bytes\n", p->tot_len);
        
        // Acknowledge received data
        tcp_recved(tpcb, p->tot_len);
        
        // Send response
        tcp_server_send_data(arg, tpcb);
        
        // Close connection
        tcp_server_close(arg);
    }
    
    pbuf_free(p);
    return ERR_OK;
}

// Handle new connection
static err_t tcp_server_accept(void *arg, struct tcp_pcb *client_pcb, err_t err) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    
    if (err != ERR_OK || client_pcb == NULL) {
        printf("Accept error\n");
        return ERR_VAL;
    }
    
    printf("Client connected\n");
    
    state->client_pcb = client_pcb;
    tcp_arg(client_pcb, state);
    tcp_recv(client_pcb, tcp_server_recv);
    
    return ERR_OK;
}

// Initialize HTTP server
void http_server_init(sd_card_t *sd_card) {
    g_sd_card = sd_card;
    
    tcp_server_state = calloc(1, sizeof(TCP_SERVER_T));
    if (!tcp_server_state) {
        printf("Failed to allocate state\n");
        return;
    }
    
    // Create new TCP PCB
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) {
        printf("Failed to create PCB\n");
        return;
    }
    
    // Bind to port
    err_t err = tcp_bind(pcb, NULL, HTTP_PORT);
    if (err) {
        printf("Failed to bind to port %d\n", HTTP_PORT);
        return;
    }
    
    // Listen for connections
    tcp_server_state->server_pcb = tcp_listen_with_backlog(pcb, 1);
    if (!tcp_server_state->server_pcb) {
        printf("Failed to listen\n");
        if (pcb) {
            tcp_close(pcb);
        }
        return;
    }
    
    // Set up accept callback
    tcp_arg(tcp_server_state->server_pcb, tcp_server_state);
    tcp_accept(tcp_server_state->server_pcb, tcp_server_accept);
    
    printf("HTTP server started on port %d\n", HTTP_PORT);
}

// Run HTTP server (non-blocking)
void http_server_run(void) {
    // The server runs in the background using lwIP callbacks
    // This function can be used for periodic maintenance if needed
}