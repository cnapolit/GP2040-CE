# Network loader for the W5500-EVB-Pico2: sits in the first 128 KB of flash, starts GP2040-CE (linked behind it), and
# takes a new GP2040-CE image over TCP when asked. Built on its own:
#   cmake -S netloader -B build-netloader -G Ninja && ninja -C build-netloader
cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "")
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(netloader C CXX ASM)
pico_sdk_init()

set(IOLIB ${CMAKE_CURRENT_LIST_DIR}/../lib/ioLibrary/ioLibrary_Driver)

add_executable(netloader
    main.c
    ${IOLIB}/Ethernet/socket.c
    ${IOLIB}/Ethernet/wizchip_conf.c
    ${IOLIB}/Ethernet/W5500/w5500.c
    ${IOLIB}/Internet/DHCP/dhcp.c
)
target_include_directories(netloader PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${IOLIB}/Ethernet
    ${IOLIB}/Ethernet/W5500
    ${IOLIB}/Internet/DHCP
)
target_compile_definitions(netloader PRIVATE _WIZCHIP_=W5500 PICO_FLASH_SIZE_BYTES=0x200000)
target_link_libraries(netloader pico_stdlib pico_unique_id hardware_spi hardware_flash hardware_watchdog)

# The loader must stay inside its 128 KB; nothing else in flash is its own.
pico_add_linker_script_override_path(netloader ${CMAKE_CURRENT_LIST_DIR}/link FILES pico_flash_region.ld)
pico_enable_stdio_usb(netloader 0)
pico_enable_stdio_uart(netloader 0)
pico_add_extra_outputs(netloader)
