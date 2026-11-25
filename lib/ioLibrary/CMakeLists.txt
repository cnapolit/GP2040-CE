# WIZnet's ioLibrary (W5500 driver, sockets and DHCP client) for the Network Input addon.
set(IOLIB ${CMAKE_CURRENT_LIST_DIR}/ioLibrary_Driver)

add_library(ioLibrary
    ${IOLIB}/Ethernet/socket.c
    ${IOLIB}/Ethernet/wizchip_conf.c
    ${IOLIB}/Ethernet/W5500/w5500.c
    ${IOLIB}/Internet/DHCP/dhcp.c
)
target_include_directories(ioLibrary PUBLIC
    ${IOLIB}/Ethernet
    ${IOLIB}/Ethernet/W5500
    ${IOLIB}/Internet/DHCP
)
# The chip must be defined the same way wherever the headers are included.
target_compile_definitions(ioLibrary PUBLIC _WIZCHIP_=W5500)
target_compile_options(ioLibrary PRIVATE -Wno-unused-variable -Wno-unused-but-set-variable -Wno-type-limits)
target_link_libraries(ioLibrary PUBLIC pico_stdlib)
