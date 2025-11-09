#include "web_output.h"
#include <string.h>
#include <stdio.h>

// Output buffer for web display
#define OUTPUT_BUFFER_SIZE 8192
static char g_web_output[OUTPUT_BUFFER_SIZE];
static int g_web_output_pos = 0;

void web_printf(const char *format, ...) {
    // Don't overflow the buffer
    if (g_web_output_pos >= OUTPUT_BUFFER_SIZE - 256) {
        return;
    }
    
    va_list args;
    va_start(args, format);
    int len = vsnprintf(g_web_output + g_web_output_pos, 
                       OUTPUT_BUFFER_SIZE - g_web_output_pos, 
                       format, args);
    va_end(args);
    
    if (len > 0) {
        g_web_output_pos += len;
    }
}

void reset_web_output(void) {
    g_web_output_pos = 0;
    g_web_output[0] = '\0';
}

const char *get_web_output(void) {
    return g_web_output;
}