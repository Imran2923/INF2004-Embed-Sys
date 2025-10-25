add_executable(spi_flash
    spi_flash.c
    hw_config.c
)

# Pull in the FatFs + SPI SD library
add_subdirectory(no-OS-FatFS-SD-SPI-RPi-Pico/FatFs_SPI build/FatFs_SPI)

target_include_directories(spi_flash PRIVATE
    no-OS-FatFS-SD-SPI-RPi-Pico/FatFs_SPI
)

# Link against required libs
target_link_libraries(spi_flash
    pico_stdlib
    hardware_spi
    FatFs_SPI
)

# ---- ENABLE USB STDIO ----
pico_enable_stdio_usb(spi_flash 1)   # turn ON USB serial
pico_enable_stdio_uart(spi_flash 0)  # turn OFF UART serial
pico_add_extra_outputs(spi_flash)

# Optional: shorten wait time for USB to enumerate (3s)
target_compile_definitions(spi_flash PRIVATE
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=3000
)

# create map/bin/hex/uf2
pico_add_extra_outputs(spi_flash)

# add url via pico_set_program_url
example_auto_set_url(spi_flash)