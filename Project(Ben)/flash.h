#ifndef FLASH_H
#define FLASH_H
#include <stdint.h>
#include <stdbool.h>

void flash_init_spi(uint32_t hz);

void cs_low(void);
void cs_high(void);

void read_jedec_id(uint8_t id[3]);
bool read_sfdp_header(uint8_t hdr8[8]);
uint8_t read_status(uint8_t which);  // 0x05 or 0x35
void write_enable(void);
void wait_wip_clear(void);

void read_data(uint32_t addr, uint8_t *buf, uint32_t len);
void page_program(uint32_t addr, const uint8_t *buf, uint32_t len);
void sector_erase_4k(uint32_t addr);

// add these only if you implement them here:
void flash_soft_reset(void);
void flash_release_from_dp(void);
void flash_recover_to_safe_mode(void);

bool backup_entire_chip(const char* filename);
bool restore_entire_chip(const char* filename);
uint32_t detect_chip_size(void);

#endif
