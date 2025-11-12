#pragma once
#include <stdbool.h>
#include <stdint.h>

void wifi_init_default(void);
bool wifi_connect_blocking(const char *ssid, const char *psk, uint32_t timeout_ms);
bool wifi_is_connected(void);
const char *wifi_get_ip_str(void);
