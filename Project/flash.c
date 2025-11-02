#include "flash.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "config.h"  // where PIN_* live

void cs_low(void)  { gpio_put(PIN_CS, 0); }
void cs_high(void) { gpio_put(PIN_CS, 1); }

void flash_init_spi(uint32_t hz){
    spi_init(spi0, hz);
    spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    cs_high();
}

void read_jedec_id(uint8_t id[3]){
    uint8_t tx[4] = {0x9F, 0, 0, 0}, rx[4] = {0};
    cs_low(); spi_write_read_blocking(spi0, tx, rx, 4); cs_high();
    id[0]=rx[1]; id[1]=rx[2]; id[2]=rx[3];
}

uint8_t read_status(uint8_t which){
    uint8_t tx[2] = {which, 0}, rx[2] = {0};
    cs_low(); spi_write_read_blocking(spi0, tx, rx, 2); cs_high();
    return rx[1];
}

bool read_sfdp_header(uint8_t hdr8[8]){
    uint8_t cmd[5] = {0x5A,0,0,0,0};
    cs_low(); spi_write_blocking(spi0, cmd, 5);
    spi_read_blocking(spi0, 0x00, hdr8, 8); cs_high();
    return hdr8[0]==0x53 && hdr8[1]==0x46 && hdr8[2]==0x44 && hdr8[3]==0x50;
}

void write_enable(void){
    uint8_t cmd=0x06; cs_low(); spi_write_blocking(spi0,&cmd,1); cs_high();
}

void wait_wip_clear(void){
    while (read_status(0x05) & 1) { sleep_ms(1); }
}

void read_data(uint32_t addr, uint8_t *buf, uint32_t len){
    uint8_t hdr[4] = {0x03, (uint8_t)(addr>>16),(uint8_t)(addr>>8),(uint8_t)addr};
    cs_low(); spi_write_blocking(spi0, hdr, 4);
    spi_read_blocking(spi0, 0x00, buf, (int)len); cs_high();
}

void page_program(uint32_t addr, const uint8_t *data, uint32_t len){
    write_enable();
    uint8_t hdr[4] = {0x02, (uint8_t)(addr>>16),(uint8_t)(addr>>8),(uint8_t)addr};
    cs_low(); spi_write_blocking(spi0, hdr, 4);
    spi_write_blocking(spi0, data, (int)len); cs_high();
    wait_wip_clear();
}

void sector_erase_4k(uint32_t addr){
    write_enable();
    uint8_t cmd[4] = {0x20,(uint8_t)(addr>>16),(uint8_t)(addr>>8),(uint8_t)addr};
    cs_low(); spi_write_blocking(spi0, cmd, 4); cs_high();
    wait_wip_clear();
}

// Many SPI NORs support JEDEC soft reset: 0x66 (Reset Enable), then 0x99 (Reset)
void flash_soft_reset(void){
    uint8_t cmd;
    cmd = 0x66; cs_low(); spi_write_blocking(spi0, &cmd, 1); cs_high();
    sleep_us(2); // tSHSL tiny gap
    cmd = 0x99; cs_low(); spi_write_blocking(spi0, &cmd, 1); cs_high();
    // give the device time to internally reset (datasheets: ~30–50us usually)
    sleep_ms(1);
}

// Some parts also wake from deep power-down with 0xAB (no harm if not in DPD)
void flash_release_from_dp(void){
    uint8_t cmd = 0xAB;
    cs_low(); spi_write_blocking(spi0, &cmd, 1); cs_high();
    sleep_us(50);
}

void flash_recover_to_safe_mode(void){
    // Try both: release-from-DP and soft-reset (harmless if not needed)
    flash_release_from_dp();
    flash_soft_reset();
    // Drop SPI speed to something very safe for JEDEC ID reads
    spi_init(spi0, SAFE_PROG_HZ);
    cs_high();
    sleep_ms(1);
}