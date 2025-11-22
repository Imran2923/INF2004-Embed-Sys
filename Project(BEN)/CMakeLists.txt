add_executable(spi_project   
    src/spi_flash.c
    src/flash.c
    bench/bench.c
    bench/csvlog.c
    bench/analyze.c
    src/ui.c
    bench/net.c
    web/http_server.c
    src/hw_config.c
    web/web_pages.c
    web/web_actions.c
    web/web_output.c
)

# Pull in the FatFs + SPI SD library
add_subdirectory(src/SDCard/FatFs_SPI)

target_include_directories(spi_project  PRIVATE    
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/src/SDCard/FatFs_SPI/include
    ${CMAKE_CURRENT_LIST_DIR}/src/SDCard/FatFs_SPI/sd_driver
    ${CMAKE_CURRENT_LIST_DIR}/src/SDCard/FatFs_SPI/ff15/source
    ${PICO_SDK_PATH}/src/rp2_common/pico_multicore/include
    ${CMAKE_CURRENT_LIST_DIR}/../
)

target_link_libraries(spi_project     
    pico_stdlib
    hardware_spi
    FatFs_SPI
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip
)

pico_enable_stdio_usb(spi_project  1)    
pico_enable_stdio_uart(spi_project  0)      

target_compile_definitions(spi_project  PRIVATE    
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=3000
)

pico_add_extra_outputs(spi_project )    
example_auto_set_url(spi_project )      