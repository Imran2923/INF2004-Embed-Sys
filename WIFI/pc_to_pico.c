/**
 * PC to Pico W - Simple WiFi Communication Demo
 * Receives data from PC and sends acknowledgment back
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "lwip/pbuf.h"
#include "lwip/tcp.h"

// Add these lines here:
#ifndef WIFI_SSID
#define WIFI_SSID "Ben10"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "xzrn7855"
#endif

#define TCP_PORT 4242
#define DEBUG_printf printf

typedef struct TCP_SERVER_T_ {
    struct tcp_pcb *server_pcb;
    struct tcp_pcb *client_pcb;
    uint8_t received_byte;
    int bytes_received;
} TCP_SERVER_T;

static TCP_SERVER_T* tcp_server_init(void) {
    TCP_SERVER_T *state = calloc(1, sizeof(TCP_SERVER_T));
    if (!state) {
        DEBUG_printf("Failed to allocate state\n");
        return NULL;
    }
    state->bytes_received = 0;
    return state;
}

static err_t tcp_server_close(void *arg) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    err_t err = ERR_OK;
    //check error
    if (state->client_pcb != NULL) {
        tcp_arg(state->client_pcb, NULL);
        tcp_poll(state->client_pcb, NULL, 0);
        tcp_sent(state->client_pcb, NULL);
        tcp_recv(state->client_pcb, NULL);
        tcp_err(state->client_pcb, NULL);
        err = tcp_close(state->client_pcb);
        if (err != ERR_OK) {
            DEBUG_printf("Close failed %d, calling abort\n", err);
            tcp_abort(state->client_pcb);
            err = ERR_ABRT;
        }
        state->client_pcb = NULL;
    }
    
    if (state->server_pcb) {
        tcp_arg(state->server_pcb, NULL);
        tcp_close(state->server_pcb);
        state->server_pcb = NULL;
    }
    
    return err;
}

// Called when data is received from PC
err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    
    if (!p) {
        DEBUG_printf("Connection closed by client\n");
        return tcp_server_close(arg);
    }
    
    cyw43_arch_lwip_check();
    
    if (p->tot_len > 0) {
        DEBUG_printf("\n=== DATA RECEIVED FROM PC ===\n");
        DEBUG_printf("Total bytes: %d\n", p->tot_len);
        
        // Print received data
        uint8_t *data = (uint8_t*)p->payload;
        for (int i = 0; i < p->tot_len; i++) {
            DEBUG_printf("  Byte[%d]: 0x%02X (decimal: %3d, char: '%c')\n", 
                        i, data[i], data[i], 
                        (data[i] >= 32 && data[i] < 127) ? data[i] : '?');
        }
        
        state->bytes_received += p->tot_len;
        state->received_byte = data[0];
        
        // Send acknowledgment back to PC
        const char *ack_msg = "ACK: Data received by Pico W!";
        err_t write_err = tcp_write(tpcb, ack_msg, strlen(ack_msg), TCP_WRITE_FLAG_COPY);
        
        if (write_err == ERR_OK) {
            tcp_output(tpcb);
            DEBUG_printf("\n>>> Sent ACK back to PC\n");
        } else {
            DEBUG_printf("Failed to send ACK: %d\n", write_err);
        }
        
        DEBUG_printf("Total bytes received: %d\n", state->bytes_received);
        DEBUG_printf("=============================\n\n");
        
        
        tcp_recved(tpcb, p->tot_len);
    }
    
    pbuf_free(p);
    return ERR_OK;
}

static err_t tcp_server_poll(void *arg, struct tcp_pcb *tpcb) {
    return ERR_OK;
}

static void tcp_server_err(void *arg, err_t err) {
    if (err != ERR_ABRT) {
        DEBUG_printf("tcp_server_err %d\n", err);
    }
}

static err_t tcp_server_accept(void *arg, struct tcp_pcb *client_pcb, err_t err) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    
    if (err != ERR_OK || client_pcb == NULL) {
        DEBUG_printf("Failure in accept\n");
        return ERR_VAL;
    }
    
    DEBUG_printf("\n================================\n");
    DEBUG_printf("  PC CONNECTED TO PICO W!\n");
    DEBUG_printf("================================\n\n");
    
    state->client_pcb = client_pcb;
    tcp_arg(client_pcb, state);
    tcp_recv(client_pcb, tcp_server_recv);
    tcp_poll(client_pcb, tcp_server_poll, 10);
    tcp_err(client_pcb, tcp_server_err);
    
    
    return ERR_OK;
}

static bool tcp_server_open(void *arg) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    
    DEBUG_printf("\n================================\n");
    DEBUG_printf("  Starting TCP Server\n");
    DEBUG_printf("================================\n");
    DEBUG_printf("IP Address: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_list)));
    DEBUG_printf("Port: %d\n", TCP_PORT);
    DEBUG_printf("================================\n\n");
    DEBUG_printf("*** COPY THE IP ADDRESS ABOVE ***\n");
    DEBUG_printf("You need it for the Python script!\n\n");
    DEBUG_printf("Waiting for PC to connect...\n\n");

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) {
        DEBUG_printf("Failed to create PCB\n");
        return false;
    }

    err_t err = tcp_bind(pcb, NULL, TCP_PORT);
    if (err) {
        DEBUG_printf("Failed to bind to port %u\n", TCP_PORT);
        return false;
    }

    state->server_pcb = tcp_listen_with_backlog(pcb, 1);
    if (!state->server_pcb) {
        DEBUG_printf("Failed to listen\n");
        if (pcb) {
            tcp_close(pcb);
        }
        return false;
    }

    tcp_arg(state->server_pcb, state);
    tcp_accept(state->server_pcb, tcp_server_accept);

    return true;
}

void run_tcp_server(void) {
    TCP_SERVER_T *state = tcp_server_init();
    if (!state) {
        return;
    }
    
    if (!tcp_server_open(state)) {
        free(state);
        return;
    }
    
    // Keep server running indefinitely
    while(true) {
        #if PICO_CYW43_ARCH_POLL
        cyw43_arch_poll();
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(1000));
        #else
        sleep_ms(100);
        #endif
    }
    
    free(state);
}

int main() {
    stdio_init_all();
    sleep_ms(2000);  // Wait for USB serial to initialize
    
    printf("\n\n");
    printf("========================================\n");
    printf("  PC to Pico W WiFi Communication\n");
    printf("========================================\n\n");

    if (cyw43_arch_init()) {
        printf("ERROR: Failed to initialize WiFi\n");
        return 1;
    }

    cyw43_arch_enable_sta_mode();

    printf("Connecting to WiFi: %s\n", WIFI_SSID);
    // allow connectivity to wifi
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, 
                                            CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        printf("ERROR: Failed to connect to WiFi\n");
        printf("Check your SSID and password!\n");
        return 1;
    }
    
    printf("SUCCESS: Connected to WiFi!\n\n");
    
    run_tcp_server();
    cyw43_arch_deinit();
    return 0;
}