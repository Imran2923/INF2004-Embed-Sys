#ifndef WEB_OUTPUT_H
#define WEB_OUTPUT_H

#include <stdarg.h>

// Web output capture system
// This allows capturing printf-style output for display in web pages

/**
 * @brief Print formatted text to web output buffer (like printf)
 * @param format Format string followed by arguments
 */
void web_printf(const char *format, ...);

/**
 * @brief Clear the web output buffer
 */
void reset_web_output(void);

/**
 * @brief Get the current web output content
 * @return Pointer to the output string
 */
const char *get_web_output(void);
void web_run_benchmark(void);
void web_run_benchmark_save(void);
void web_run_benchmark_100(void);
void web_show_status(void);

#endif // WEB_OUTPUT_H