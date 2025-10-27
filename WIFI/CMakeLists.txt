if (NOT DEFINED WIFI_SSID)
    set(WIFI_SSID "your_wifi_ssid")
    message("Using default WIFI_SSID: ${WIFI_SSID}")
endif()

if (NOT DEFINED WIFI_PASSWORD)
    set(WIFI_PASSWORD "your_wifi_password")
    message("Using default WIFI_PASSWORD")
endif()

add_executable(pc_to_pico
    pc_to_pico.c
)

target_compile_definitions(pc_to_pico PRIVATE
    WIFI_SSID=\"${WIFI_SSID}\"
    WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
)

target_include_directories(pc_to_pico PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../ # Add parent directory for lwipopts
)

target_link_libraries(pc_to_pico
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
    hardware_gpio  # Add this for GPIO support
)

pico_add_extra_outputs(pc_to_pico)

pico_enable_stdio_usb(pc_to_pico 1)
pico_enable_stdio_uart(pc_to_pico 0)

example_auto_set_url(pc_to_pico)