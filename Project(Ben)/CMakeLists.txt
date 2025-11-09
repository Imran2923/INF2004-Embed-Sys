add_executable(my_sd_project    # Change from spi_flash to something unique
    spi_flash.c
    flash.c
    bench.c
    csvlog.c
    analyze.c
    ui.c
    net.c
    http_server.c
    hw_config.c
    web_pages.c
    web_actions.c
    web_output.c
)

# Pull in the FatFs + SPI SD library
add_subdirectory(SDCard/FatFs_SPI)

target_include_directories(my_sd_project PRIVATE    # Update here
    SDCard/FatFs_SPI 
    SDCard/FatFs_SPI/source
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../
)

target_link_libraries(my_sd_project    # Update here
    pico_stdlib
    hardware_spi
    FatFs_SPI
    pico_cyw43_arch_lwip_threadsafe_background
)

pico_enable_stdio_usb(my_sd_project 1)    # Update here
pico_enable_stdio_uart(my_sd_project 0)   # Update here
pico_add_extra_outputs(my_sd_project)     # Update here

target_compile_definitions(my_sd_project PRIVATE    # Update here
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=3000
)

pico_add_extra_outputs(my_sd_project)    # Update here
example_auto_set_url(my_sd_project)      # Update here