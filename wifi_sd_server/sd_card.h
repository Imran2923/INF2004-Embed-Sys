#ifndef SD_CARD_H
#define SD_CARD_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware/spi.h"

// SD Card SPI Configuration for Maker Pi Pico
#define SD_SPI_PORT spi1
#define SD_PIN_MISO 12
#define SD_PIN_CS   15
#define SD_PIN_SCK  10
#define SD_PIN_MOSI 11

// SD Card commands
#define CMD0    0   // GO_IDLE_STATE
#define CMD1    1   // SEND_OP_COND
#define CMD8    8   // SEND_IF_COND
#define CMD9    9   // SEND_CSD
#define CMD10   10  // SEND_CID
#define CMD12   12  // STOP_TRANSMISSION
#define CMD16   16  // SET_BLOCKLEN
#define CMD17   17  // READ_SINGLE_BLOCK
#define CMD18   18  // READ_MULTIPLE_BLOCK
#define CMD23   23  // SET_BLOCK_COUNT
#define CMD24   24  // WRITE_BLOCK
#define CMD25   25  // WRITE_MULTIPLE_BLOCK
#define CMD55   55  // APP_CMD
#define CMD58   58  // READ_OCR
#define ACMD41  41  // SEND_OP_COND (SDC)

// SD Card response types
#define R1_IDLE_STATE           (1 << 0)
#define R1_ERASE_RESET          (1 << 1)
#define R1_ILLEGAL_COMMAND      (1 << 2)
#define R1_COM_CRC_ERROR        (1 << 3)
#define R1_ERASE_SEQUENCE_ERROR (1 << 4)
#define R1_ADDRESS_ERROR        (1 << 5)
#define R1_PARAMETER_ERROR      (1 << 6)

// Data tokens
#define TOKEN_START_BLOCK       0xFE
#define TOKEN_STOP_TRAN         0xFD

// Block size
#define SD_BLOCK_SIZE           512

// SD Card type
typedef enum {
    SD_CARD_TYPE_UNKNOWN = 0,
    SD_CARD_TYPE_SD1,
    SD_CARD_TYPE_SD2,
    SD_CARD_TYPE_SDHC
} sd_card_type_t;

// SD Card structure
typedef struct {
    spi_inst_t *spi;
    uint8_t cs_pin;
    sd_card_type_t type;
    uint32_t sectors;
    bool initialized;
} sd_card_t;

// Function prototypes
bool sd_init(sd_card_t *sd);
bool sd_read_block(sd_card_t *sd, uint32_t block, uint8_t *buffer);
bool sd_write_block(sd_card_t *sd, uint32_t block, const uint8_t *buffer);
uint32_t sd_get_num_sectors(sd_card_t *sd);

#endif // SD_CARD_H