// Network loader for the WIZnet W5500-EVB-Pico2 (RP2350, 2 MB flash).
//
// On boot it starts the firmware at NETLOADER_APP_OFFSET unless the firmware asked for an update (watchdog scratch)
// or there is no valid firmware. Otherwise it brings up the W5500 (DHCP, else a link-local address), waits for an
// image on NETLOADER_TCP_PORT, writes it, and starts it. The image's first sector is written last, so an update that
// is cut off leaves no valid firmware and the board stays here, ready to try again. The loader itself is never
// rewritten over the network: replacing it takes USB and BOOTSEL.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/structs/nvic.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/systick.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"

#include "dhcp.h"
#include "socket.h"
#include "wizchip_conf.h"

#include "netloader.h"

#define W5500_SPI spi0
#define PIN_MISO 16
#define PIN_CS 17
#define PIN_SCK 18
#define PIN_MOSI 19
#define PIN_RST 20
#define PIN_LED 25

#define SOCK_DHCP 0
#define SOCK_TCP 1
#define SOCK_STATUS 2
#define STATUS_UDP_PORT 7284

// Stay this long for an update the firmware asked for, then go back to the firmware.
#define UPDATE_WAIT_MS (5 * 60 * 1000)
#define DHCP_TIMEOUT_MS 15000

enum loader_state { STATE_WAITING = 0, STATE_RECEIVING = 1, STATE_FAILED = 2 };

static uint8_t dhcp_buf[548];
static uint8_t sector[FLASH_SECTOR_SIZE];
static uint8_t first_sector[FLASH_SECTOR_SIZE];
static uint32_t crc_table[256];

static enum loader_state state = STATE_WAITING;
static uint32_t received_total = 0;
static uint32_t expected_total = 0;
static uint8_t last_error = 0;

// --- W5500 glue ---

static void cs_select(void) { gpio_put(PIN_CS, 0); }
static void cs_deselect(void) { gpio_put(PIN_CS, 1); }
static uint8_t spi_rb(void) {
    uint8_t b;
    spi_read_blocking(W5500_SPI, 0x00, &b, 1);
    return b;
}
static void spi_wb(uint8_t b) { spi_write_blocking(W5500_SPI, &b, 1); }
static void spi_rbuf(uint8_t *buf, uint16_t len) { spi_read_blocking(W5500_SPI, 0x00, buf, len); }
static void spi_wbuf(uint8_t *buf, uint16_t len) { spi_write_blocking(W5500_SPI, buf, len); }

static bool w5500_setup(void) {
    spi_init(W5500_SPI, 33 * 1000 * 1000);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);
    gpio_init(PIN_RST);
    gpio_set_dir(PIN_RST, GPIO_OUT);
    gpio_put(PIN_RST, 0);
    sleep_ms(2);
    gpio_put(PIN_RST, 1);
    sleep_ms(50);

    reg_wizchip_cs_cbfunc(cs_select, cs_deselect);
    reg_wizchip_spi_cbfunc(spi_rb, spi_wb);
    reg_wizchip_spiburst_cbfunc(spi_rbuf, spi_wbuf);

    // DHCP and TCP get the most buffer; the status socket a little.
    uint8_t sizes[2][8] = {{2, 8, 2, 0, 0, 0, 0, 0}, {2, 8, 2, 0, 0, 0, 0, 0}};
    if (ctlwizchip(CW_INIT_WIZCHIP, sizes) != 0 || getVERSIONR() != 0x04) {
        return false;
    }

    // The same MAC as the firmware's, so DHCP hands out the same address.
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    uint8_t mac[6] = {0x02};
    memcpy(&mac[1], &id.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 5], 5);
    setSHAR(mac);
    return true;
}

static bool link_up(void) {
    uint8_t link = PHY_LINK_OFF;
    ctlwizchip(CW_GET_PHYLINK, &link);
    return link == PHY_LINK_ON;
}

static void take_link_local_address(void) {
    DHCP_stop();
    uint8_t mac[6];
    getSHAR(mac);
    uint8_t ip[4] = {169, 254, mac[4] ? mac[4] : 1, mac[5] == 0 || mac[5] == 255 ? 1 : mac[5]};
    uint8_t sn[4] = {255, 255, 0, 0};
    uint8_t gw[4] = {0, 0, 0, 0};
    setSIPR(ip);
    setSUBR(sn);
    setGAR(gw);
}

// --- Images ---

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        crc_table[i] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc = crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

static bool app_valid(void) {
    const uint32_t *vectors = (const uint32_t *)(XIP_BASE + NETLOADER_APP_OFFSET);
    uint32_t sp = vectors[0];
    uint32_t reset = vectors[1];
    return sp >= SRAM_BASE && sp <= SRAM_END && (reset & 1) &&
           reset >= XIP_BASE + NETLOADER_APP_OFFSET && reset < XIP_BASE + NETLOADER_APP_END;
}

static void boot_app(void) {
    const uint32_t *vectors = (const uint32_t *)(XIP_BASE + NETLOADER_APP_OFFSET);
    (void)save_and_disable_interrupts();
    systick_hw->csr = 0;
    for (int i = 0; i < 2; i++) {
        nvic_hw->icer[i] = 0xFFFFFFFFu;
        nvic_hw->icpr[i] = 0xFFFFFFFFu;
    }
    scb_hw->vtor = XIP_BASE + NETLOADER_APP_OFFSET;
    __asm volatile(
        "msr msp, %0\n"
        "cpsie i\n"
        "bx %1\n" ::"r"(vectors[0]),
        "r"(vectors[1]));
    __builtin_unreachable();
}

static void flash_erase(uint32_t offset, size_t length) {
    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(offset, length);
    restore_interrupts(irq);
}

static void flash_program(uint32_t offset, const uint8_t *data, size_t length) {
    uint32_t irq = save_and_disable_interrupts();
    flash_range_program(offset, data, length);
    restore_interrupts(irq);
}

// --- Network ---

static bool recv_exact(uint8_t *buf, uint32_t len) {
    uint32_t got = 0;
    while (got < len) {
        uint8_t status = getSn_SR(SOCK_TCP);
        if (status != SOCK_ESTABLISHED && getSn_RX_RSR(SOCK_TCP) == 0) {
            return false;
        }
        if (getSn_RX_RSR(SOCK_TCP) == 0) {
            continue;
        }
        int32_t n = recv(SOCK_TCP, buf + got, len - got > 2048 ? 2048 : len - got);
        if (n <= 0) {
            return false;
        }
        got += (uint32_t)n;
    }
    return true;
}

static void reply(const char *text) {
    send(SOCK_TCP, (uint8_t *)text, (uint16_t)strlen(text));
}

static void fail(const char *text, uint8_t code) {
    reply(text);
    state = STATE_FAILED;
    last_error = code;
    disconnect(SOCK_TCP);
}

// Takes one image over the established connection. Returns only on failure.
static void receive_image(void) {
    uint32_t header[4];
    if (!recv_exact((uint8_t *)header, sizeof header)) {
        fail("ERR header\n", 1);
        return;
    }
    if (header[0] != NETLOADER_HEADER_MAGIC) {
        fail("ERR magic\n", 2);
        return;
    }
    uint32_t length = header[1];
    uint32_t want_crc = header[2];
    if (length <= FLASH_SECTOR_SIZE || length > NETLOADER_APP_LENGTH) {
        fail("ERR length\n", 3);
        return;
    }

    state = STATE_RECEIVING;
    expected_total = length;
    received_total = 0;

    // Erasing the first sector first makes the old firmware invalid until the new one is complete.
    uint32_t span = (length + FLASH_SECTOR_SIZE - 1) & ~(FLASH_SECTOR_SIZE - 1);
    flash_erase(NETLOADER_APP_OFFSET, span);
    reply("ERASED\n");

    uint32_t crc = 0;
    for (uint32_t offset = 0; offset < length; offset += FLASH_SECTOR_SIZE) {
        uint32_t chunk = length - offset < FLASH_SECTOR_SIZE ? length - offset : FLASH_SECTOR_SIZE;
        memset(sector, 0xFF, sizeof sector);
        if (!recv_exact(sector, chunk)) {
            fail("ERR short\n", 4);
            return;
        }
        crc = crc_update(crc, sector, chunk);
        received_total = offset + chunk;
        if (offset == 0) {
            memcpy(first_sector, sector, sizeof first_sector);
        } else {
            flash_program(NETLOADER_APP_OFFSET + offset, sector, FLASH_SECTOR_SIZE);
        }
        gpio_put(PIN_LED, (offset / FLASH_SECTOR_SIZE) & 1);
    }

    if (crc != want_crc) {
        fail("ERR crc\n", 5);
        return;
    }

    // Check what landed in flash (all but the first sector) against what arrived, then commit the first sector.
    uint32_t check = crc_update(0, first_sector, length < FLASH_SECTOR_SIZE ? length : FLASH_SECTOR_SIZE);
    check = crc_update(check, (const uint8_t *)(XIP_BASE + NETLOADER_APP_OFFSET + FLASH_SECTOR_SIZE),
                       length - FLASH_SECTOR_SIZE);
    if (check != want_crc) {
        fail("ERR verify\n", 6);
        return;
    }
    flash_program(NETLOADER_APP_OFFSET, first_sector, FLASH_SECTOR_SIZE);

    reply("OK\n");
    sleep_ms(100);
    disconnect(SOCK_TCP);
    watchdog_hw->scratch[NETLOADER_SCRATCH_INDEX] = 0;
    watchdog_reboot(0, 0, 10);
    while (true) {
        tight_loop_contents();
    }
}

static void answer_status(void) {
    if (getSn_SR(SOCK_STATUS) != SOCK_UDP) {
        socket(SOCK_STATUS, Sn_MR_UDP, STATUS_UDP_PORT, 0);
        return;
    }
    while (getSn_RX_RSR(SOCK_STATUS) > 0) {
        uint8_t buf[64];
        uint8_t ip[4];
        uint16_t port;
        int32_t n = recvfrom(SOCK_STATUS, buf, sizeof buf, ip, &port);
        uint32_t magic = 0;
        if (n >= 4) {
            memcpy(&magic, buf, 4);
        }
        if (magic != 0x51504C53u) { // "SLPQ"
            continue;
        }
        uint32_t reply_words[5] = {NETLOADER_STATUS_MAGIC, state | (last_error << 8) | (app_valid() << 16),
                                   received_total, expected_total, to_ms_since_boot(get_absolute_time())};
        sendto(SOCK_STATUS, (uint8_t *)reply_words, sizeof reply_words, ip, port);
    }
}

int main(void) {
    bool asked = watchdog_hw->scratch[NETLOADER_SCRATCH_INDEX] == NETLOADER_UPDATE_MAGIC;
    if (!asked && app_valid()) {
        boot_app();
    }

    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    crc_init();

    if (!w5500_setup()) {
        // Without the W5500 there is nothing to wait for; the firmware (if any) is better than nothing.
        if (app_valid()) {
            watchdog_hw->scratch[NETLOADER_SCRATCH_INDEX] = 0;
            boot_app();
        }
        while (true) {
            gpio_put(PIN_LED, (to_ms_since_boot(get_absolute_time()) / 100) % 2);
        }
    }

    DHCP_init(SOCK_DHCP, dhcp_buf);
    bool have_ip = false;
    bool link_local = false;
    uint32_t start = to_ms_since_boot(get_absolute_time());
    uint32_t last_tick = start;

    while (true) {
        uint32_t now = to_ms_since_boot(get_absolute_time());

        // Fast blink: waiting for an image.
        gpio_put(PIN_LED, (now / 100) % 2);

        if (asked && app_valid() && state == STATE_WAITING && now - start > UPDATE_WAIT_MS) {
            watchdog_hw->scratch[NETLOADER_SCRATCH_INDEX] = 0;
            watchdog_reboot(0, 0, 10);
            while (true) {
                tight_loop_contents();
            }
        }

        if (!link_up()) {
            continue;
        }

        if (!link_local) {
            if (now - last_tick >= 1000) {
                last_tick += 1000;
                DHCP_time_handler();
            }
            uint8_t dhcp = DHCP_run();
            if (dhcp == DHCP_IP_ASSIGN || dhcp == DHCP_IP_CHANGED || dhcp == DHCP_IP_LEASED) {
                have_ip = true;
            }
            if (!have_ip && now - start >= DHCP_TIMEOUT_MS) {
                take_link_local_address();
                link_local = true;
                have_ip = true;
            }
        }
        if (!have_ip) {
            continue;
        }

        answer_status();

        switch (getSn_SR(SOCK_TCP)) {
        case SOCK_CLOSED:
            socket(SOCK_TCP, Sn_MR_TCP, NETLOADER_TCP_PORT, 0);
            break;
        case SOCK_INIT:
            listen(SOCK_TCP);
            break;
        case SOCK_ESTABLISHED:
            receive_image();
            break;
        case SOCK_CLOSE_WAIT:
            disconnect(SOCK_TCP);
            break;
        default:
            break;
        }
    }
}
