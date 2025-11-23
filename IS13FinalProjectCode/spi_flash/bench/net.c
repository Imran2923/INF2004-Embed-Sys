// net.c
#include "net.h"
#include "http_server.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/dhcp.h"
#include "lwip/netif.h"      // <-- add
#include "lwip/ip4_addr.h"   // <-- add
#include <stdio.h>

static bool s_inited = false;

void wifi_init_default(void) {
    if (s_inited) return;
    if (cyw43_arch_init()) {
        printf("ERROR: cyw43_arch_init failed\n");
        return;
    }
    cyw43_arch_enable_sta_mode();
    s_inited = true;
}

/* Returns true when STA link is up (association completed). */
bool wifi_is_connected(void) {
    if (!s_inited) return false;
    return cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP;
}

const char *wifi_get_ip_str(void) {
    static char buf[32];
    const ip4_addr_t *a = netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
    ip4addr_ntoa_r(a, buf, sizeof buf);
    return buf;
}

bool wifi_connect_blocking(const char *ssid, const char *psk, uint32_t timeout_ms) {
    wifi_init_default();
    printf("connect status: joining\n");

    int r = cyw43_arch_wifi_connect_timeout_ms(ssid, psk, CYW43_AUTH_WPA2_AES_PSK, timeout_ms);
    if (r) {
        printf("connect status: failed (%d)\n", r);
        return false;
    }

    // At this point the link is up (associated) but we still may not have an IPv4.
    printf("connect status: link up\n");

#if LWIP_DHCP
    struct netif *nif = &cyw43_state.netif[CYW43_ITF_STA];

    // Ensure a clean start, then kick DHCP.
    dhcp_stop(nif);
    ip4_addr_set_zero(ip_2_ip4(&nif->ip_addr));
    ip4_addr_set_zero(ip_2_ip4(&nif->netmask));
    ip4_addr_set_zero(ip_2_ip4(&nif->gw));
    dhcp_start(nif);

    // Wait up to ~10s for an address
    const absolute_time_t deadline = make_timeout_time_ms(10000);
    while (ip4_addr_isany_val(*netif_ip4_addr(nif))) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
            printf("connect status: no ip (DHCP timeout)\n");
            return false;
        }
        sleep_ms(50);
    }
    printf("connect status: got ip %s\n", ip4addr_ntoa(netif_ip4_addr(nif)));
#else
    printf("connect status: no ip (DHCP disabled)\n");
#endif

    return true;
}
