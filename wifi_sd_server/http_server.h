#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "sd_card.h"

// HTTP Server Configuration
#define HTTP_PORT 80
#define MAX_PATH_LENGTH 256
#define MAX_FILES 50

// File info structure
typedef struct {
    char name[128];
    uint32_t size;
    bool is_directory;
} file_info_t;

// Function prototypes
void http_server_init(sd_card_t *sd_card);
void http_server_run(void);

#endif // HTTP_SERVER_H