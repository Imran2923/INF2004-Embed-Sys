#ifndef WEB_PAGES_H
#define WEB_PAGES_H

#include "lwip/tcp.h"
#include <stdbool.h>
// HTML page generation functions
// These create and send complete web pages to the browser

/**
 * @brief Send the home page with upload form and status
 */
void send_home_page(struct tcp_pcb *pcb);

/**
 * @brief Send the web control menu with all action buttons
 */
void send_web_menu(struct tcp_pcb *pcb);

/**
 * @brief Send the action result page with captured output
 * @param cmd The command that was executed
 */
void send_action_result_page(struct tcp_pcb *pcb, const char *cmd);

/**
 * @brief Send directory listing for SD card browsing
 * @param path_qs URL-encoded path parameter
 */
void send_dir_listing(struct tcp_pcb *pcb, const char *path_qs);

/**
 * @brief Send file download (raw file data)
 * @param path_qs URL-encoded path parameter
 */
void send_file_download(struct tcp_pcb *pcb, const char *path_qs);

/**
 * @brief Send upload success/failure response
 * @param filename Name of uploaded file
 * @param bytes_received Size of uploaded file
 * @param success Whether upload succeeded
 */
void send_upload_response(struct tcp_pcb *pcb, const char *filename, uint32_t bytes_received, bool success);
bool sd_ok(void);

#endif // WEB_PAGES_H