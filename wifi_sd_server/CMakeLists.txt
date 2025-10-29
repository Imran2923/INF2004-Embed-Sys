add_executable(wifi_sd_server
    main.c
    sd_card.c
    http_server.c
)

target_include_directories(wifi_sd_server PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../  # Add parent directory for lwipopts
)

target_link_libraries(wifi_sd_server
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
    hardware_spi
    hardware_gpio  # Add GPIO support
)

pico_enable_stdio_usb(wifi_sd_server 1)
pico_enable_stdio_uart(wifi_sd_server 0)

pico_add_extra_outputs(wifi_sd_server)