#include "addons/network_input.h"

#include <string.h>

#include "storagemanager.h"
#include "drivermanager.h"
#include "gamepad.h"
#include "drivers/switchpro/SwitchProDriver.h"
#include "tusb.h"
#include "usbdriver.h"
#include "system.h"
#include "hardware/watchdog.h"
#ifdef NETLOADER_PRESENT
#include "netloader.h"
#endif

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/structs/usb.h"

extern "C" {
#include "wizchip_conf.h"
#include "socket.h"
#include "dhcp.h"
}

namespace {

constexpr uint8_t SOCK_DHCP = 0;
constexpr uint8_t SOCK_PAD = 1;

uint8_t dhcpBuffer[548];
uint8_t rxBuffer[64];
constexpr uint32_t PIN_VBUS_SENSE = 24;
constexpr uint32_t PIN_LED = 25;

void csSelect() { gpio_put(NETWORK_INPUT_PIN_CS, 0); }
void csDeselect() { gpio_put(NETWORK_INPUT_PIN_CS, 1); }

uint8_t spiReadByte() {
    uint8_t b;
    spi_read_blocking(NETWORK_INPUT_SPI_BLOCK, 0x00, &b, 1);
    return b;
}

void spiWriteByte(uint8_t b) { spi_write_blocking(NETWORK_INPUT_SPI_BLOCK, &b, 1); }
void spiReadBurst(uint8_t *buf, uint16_t len) { spi_read_blocking(NETWORK_INPUT_SPI_BLOCK, 0x00, buf, len); }
void spiWriteBurst(uint8_t *buf, uint16_t len) { spi_write_blocking(NETWORK_INPUT_SPI_BLOCK, buf, len); }

uint32_t nowMs() { return to_ms_since_boot(get_absolute_time()); }

bool linkUp() {
    uint8_t link = PHY_LINK_OFF;
    ctlwizchip(CW_GET_PHYLINK, &link);
    return link == PHY_LINK_ON;
}

} // namespace

bool NetworkInput::available() {
    return NETWORK_INPUT_ENABLED;
}

void NetworkInput::setup() {
    gpio_init(PIN_VBUS_SENSE);
    gpio_set_dir(PIN_VBUS_SENSE, GPIO_IN);
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);

    // Hold the W5500 in reset (quiet, low power) until startChip().
    gpio_init(NETWORK_INPUT_PIN_RST);
    gpio_set_dir(NETWORK_INPUT_PIN_RST, GPIO_OUT);
    gpio_put(NETWORK_INPUT_PIN_RST, 0);
    if (NETWORK_INPUT_START_DELAY_MS == 0) {
        startChip();
    }
}

void NetworkInput::startChip() {
    chipStarted = true;
    spi_init(NETWORK_INPUT_SPI_BLOCK, 33 * 1000 * 1000);
    gpio_set_function(NETWORK_INPUT_PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(NETWORK_INPUT_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(NETWORK_INPUT_PIN_MOSI, GPIO_FUNC_SPI);

    gpio_init(NETWORK_INPUT_PIN_CS);
    gpio_set_dir(NETWORK_INPUT_PIN_CS, GPIO_OUT);
    gpio_put(NETWORK_INPUT_PIN_CS, 1);

    gpio_put(NETWORK_INPUT_PIN_RST, 0);
    sleep_ms(2);
    gpio_put(NETWORK_INPUT_PIN_RST, 1);
    sleep_ms(50);

    reg_wizchip_cs_cbfunc(csSelect, csDeselect);
    reg_wizchip_spi_cbfunc(spiReadByte, spiWriteByte);
    reg_wizchip_spiburst_cbfunc(spiReadBurst, spiWriteBurst);

    // Sockets 0 (DHCP) and 1 (pad state) get 4 KB each; the rest none.
    uint8_t sizes[2][8] = {{4, 4, 0, 0, 0, 0, 0, 0}, {4, 4, 0, 0, 0, 0, 0, 0}};
    chipFound = ctlwizchip(CW_INIT_WIZCHIP, sizes) == 0 && getVERSIONR() == 0x04;
    if (!chipFound) {
        return;
    }

    // A locally administered MAC from the flash chip's unique id, so each board keeps its own.
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    uint8_t mac[6] = {0x02};
    memcpy(&mac[1], &id.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 5], 5);
    setSHAR(mac);

    DHCP_init(SOCK_DHCP, dhcpBuffer);
    dhcpStartedAt = lastDhcpTick = nowMs();
}

void NetworkInput::takeLinkLocalAddress() {
    DHCP_stop();
    uint8_t mac[6];
    getSHAR(mac);
    uint8_t ip[4] = {169, 254, static_cast<uint8_t>(mac[4] ? mac[4] : 1),
                     static_cast<uint8_t>(mac[5] == 0 || mac[5] == 255 ? 1 : mac[5])};
    uint8_t sn[4] = {255, 255, 0, 0};
    uint8_t gw[4] = {0, 0, 0, 0};
    setSIPR(ip);
    setSUBR(sn);
    setGAR(gw);
    linkLocal = true;
}

void NetworkInput::pollNetwork(uint32_t now) {
    if (!linkUp()) {
        return;
    }

    if (!linkLocal) {
        if (now - lastDhcpTick >= 1000) {
            lastDhcpTick += 1000;
            DHCP_time_handler();
        }
        switch (DHCP_run()) {
        case DHCP_IP_ASSIGN:
        case DHCP_IP_CHANGED:
        case DHCP_IP_LEASED:
            haveAddress = true;
            break;
        case DHCP_FAILED:
            haveAddress = false;
            break;
        default:
            break;
        }
        if (!haveAddress && now - dhcpStartedAt >= NETWORK_INPUT_DHCP_TIMEOUT_MS) {
            takeLinkLocalAddress();
        }
    }

    if (linkLocal) {
        haveAddress = true;
    }
    if (!haveAddress) {
        return;
    }

    if (getSn_SR(SOCK_PAD) != SOCK_UDP) {
        socket(SOCK_PAD, Sn_MR_UDP, NETWORK_INPUT_UDP_PORT, 0);
        return;
    }
    receive(now);
}

void NetworkInput::receive(uint32_t now) {
    // Drain everything queued and keep only the newest state.
    while (getSn_RX_RSR(SOCK_PAD) > 0) {
        uint8_t fromIp[4];
        uint16_t fromPort;
        int32_t n = recvfrom(SOCK_PAD, rxBuffer, sizeof rxBuffer, fromIp, &fromPort);
        uint32_t magic = 0;
        if (n >= 4) {
            memcpy(&magic, rxBuffer, 4);
        }
        if (magic == NETWORK_INPUT_QUERY_MAGIC) {
            sendStatus(fromIp, fromPort, now);
            continue;
        }
        if (magic == NETWORK_INPUT_PAIR_MAGIC && n >= static_cast<int32_t>(sizeof(NetworkPairPacket))) {
            NetworkPairPacket packet;
            memcpy(&packet, rxBuffer, sizeof packet);
            pair(packet, fromIp, fromPort, now);
            continue;
        }
        if (magic == NETWORK_INPUT_COMMAND_MAGIC && n >= static_cast<int32_t>(sizeof(NetworkCommandPacket))) {
            NetworkCommandPacket packet;
            memcpy(&packet, rxBuffer, sizeof packet);
            if (keyMatches(packet.key)) {
                command(packet);
            }
            continue;
        }
        if (n < static_cast<int32_t>(sizeof(NetworkPadPacket))) {
            continue;
        }
        NetworkPadPacket packet;
        memcpy(&packet, rxBuffer, sizeof packet);
        if (packet.magic != NETWORK_INPUT_MAGIC || packet.version != NETWORK_INPUT_VERSION || !paired() ||
            !keyMatches(packet.key)) {
            continue;
        }

        // A new sender, or one silent past the timeout, starts a fresh sequence; otherwise drop reordered packets.
        bool sameSender = havePad && memcmp(fromIp, senderIp, 4) == 0 && fromPort == senderPort &&
                          now - lastPacketAt < NETWORK_INPUT_TIMEOUT_MS;
        if (sameSender && static_cast<int32_t>(packet.sequence - lastSequence) <= 0) {
            continue;
        }

        padPackets++;
        memcpy(senderIp, fromIp, 4);
        senderPort = fromPort;
        lastSequence = packet.sequence;
        lastPacketAt = now;
        pad = packet;
        havePad = true;
    }
}

bool NetworkInput::paired() const {
    const Config &config = Storage::getInstance().getConfig();
    return config.has_networkInputKey && config.networkInputKey.size == NETWORK_INPUT_KEY_SIZE;
}

bool NetworkInput::pairingOpen(uint32_t now) const {
    return !paired() || now < NETWORK_INPUT_PAIR_WINDOW_MS;
}

// An unpaired board takes commands from anyone, as anyone could pair it; a paired one only with its key.
bool NetworkInput::keyMatches(const uint8_t key[NETWORK_INPUT_KEY_SIZE]) const {
    if (!paired()) {
        return true;
    }
    const uint8_t *stored = Storage::getInstance().getConfig().networkInputKey.bytes;
    uint8_t diff = 0;
    for (size_t i = 0; i < NETWORK_INPUT_KEY_SIZE; i++) {
        diff |= stored[i] ^ key[i];
    }
    return diff == 0;
}

void NetworkInput::pair(const NetworkPairPacket &packet, uint8_t ip[4], uint16_t port, uint32_t now) {
    if (!pairingOpen(now)) {
        sendStatus(ip, port, now);
        return;
    }
    Config &config = Storage::getInstance().getConfig();
    bool same = paired() && memcmp(config.networkInputKey.bytes, packet.key, NETWORK_INPUT_KEY_SIZE) == 0;
    if (!same) {
        memcpy(config.networkInputKey.bytes, packet.key, NETWORK_INPUT_KEY_SIZE);
        config.networkInputKey.size = NETWORK_INPUT_KEY_SIZE;
        config.has_networkInputKey = true;
        Storage::getInstance().save(true);
        // Whoever sent pad state before is no longer paired.
        havePad = false;
    }
    sendStatus(ip, port, now);
}

void NetworkInput::command(const NetworkCommandPacket &packet) {
    switch (packet.command) {
    case NETWORK_INPUT_COMMAND_SET_INPUT_MODE:
        Storage::getInstance().getGamepadOptions().inputMode = static_cast<InputMode>(packet.argument);
        Storage::getInstance().save(true);
        System::reboot(System::BootMode::DEFAULT);
        break;
    case NETWORK_INPUT_COMMAND_REBOOT:
        System::reboot(System::BootMode::DEFAULT);
        break;
    case NETWORK_INPUT_COMMAND_BOOTSEL:
        System::reboot(System::BootMode::USB);
        break;
#ifdef NETLOADER_PRESENT
    case NETWORK_INPUT_COMMAND_UPDATE:
        watchdog_hw->scratch[NETLOADER_SCRATCH_INDEX] = NETLOADER_UPDATE_MAGIC;
        watchdog_reboot(0, 0, 10);
        break;
#endif
    default:
        break;
    }
}

void NetworkInput::sendStatus(uint8_t ip[4], uint16_t port, uint32_t now) {
    NetworkStatusPacket status = {};
    status.magic = NETWORK_INPUT_STATUS_MAGIC;
    status.version = NETWORK_INPUT_VERSION;
    InputMode mode = DriverManager::getInstance().getInputMode();
    status.inputMode = static_cast<uint8_t>(mode);
    status.usbMounted = tud_mounted();
    status.usbSuspended = tud_suspended();
    status.uptimeMs = now;
    status.padPackets = padPackets;
    status.msSinceLastPad = padPackets ? now - lastPacketAt : UINT32_MAX;
    GPDriver *driver = DriverManager::getInstance().getDriver();
    if (mode == INPUT_MODE_SWITCH_PRO && driver != nullptr) {
        auto *pro = static_cast<SwitchProDriver *>(driver);
        SwitchProDriver::DebugInfo debug = pro->debugInfo();
        status.switchReady = pro->ready();
        status.switchInitialized = pro->initialized();
        status.switchPlayer = pro->player();
        status.switchReportCounter = pro->reportCounter();
        status.switchSetReports = debug.setReports;
        status.switchLastReportId = debug.lastReportId;
        status.switchLastSubId = debug.lastSubId;
        status.switchLastSubcommand = debug.lastSubcommand;
    }
    status.vbus = gpio_get(PIN_VBUS_SENSE);
    status.usbDeviceDescriptors = usbDebug.deviceDescriptors;
    status.usbConfigDescriptors = usbDebug.configDescriptors;
    status.usbHidReportDescriptors = usbDebug.hidReportDescriptors;
    status.usbStrings = usbDebug.strings;
    status.usbMounts = usbDebug.mounts;
    status.usbUnmounts = usbDebug.unmounts;
    status.usbSuspends = usbDebug.suspends;
    status.sieStatus = usb_hw->sie_status;
    status.sieCtrl = usb_hw->sie_ctrl;
    status.usbPwr = usb_hw->pwr;
    status.tudConnected = tud_connected();
    memcpy(status.lineStateSamples, lineStateSamples, sizeof lineStateSamples);
    memcpy(status.usbEvents, usbDebug.events, sizeof status.usbEvents);
    getSHAR(status.mac);
    status.paired = paired();
    status.pairingOpen = pairingOpen(now);
    sendto(SOCK_PAD, reinterpret_cast<uint8_t *>(&status), sizeof status, ip, port);
}

void NetworkInput::process() {
    uint32_t now = nowMs();
    lineStateSamples[(usb_hw->sie_status & USB_SIE_STATUS_LINE_STATE_BITS) >> USB_SIE_STATUS_LINE_STATE_LSB]++;

    // On-board LED: on while a host has set up USB, blinking while there is 5 V but no host, off without 5 V.
    bool vbus = gpio_get(PIN_VBUS_SENSE);
    gpio_put(PIN_LED, tud_mounted() || (vbus && (now / 250) % 2));

#if NETWORK_INPUT_START_DELAY_MS > 0
    if (!chipStarted && now >= NETWORK_INPUT_START_DELAY_MS) {
        startChip();
    }
#endif
    if (!chipFound) {
        return;
    }

    pollNetwork(now);

    if (havePad && now - lastPacketAt >= NETWORK_INPUT_TIMEOUT_MS) {
        havePad = false;
    }

    Gamepad *gamepad = Storage::getInstance().GetGamepad();
    if (!havePad) {
        // Leave whatever the board's own pins produced; the network adds nothing.
        return;
    }

    gamepad->state.buttons |= pad.buttons;
    gamepad->state.dpad |= pad.dpad & GAMEPAD_MASK_DPAD;
    gamepad->state.lx = pad.lx;
    gamepad->state.ly = pad.ly;
    gamepad->state.rx = pad.rx;
    gamepad->state.ry = pad.ry;
    gamepad->state.lt = pad.lt;
    gamepad->state.rt = pad.rt;
    gamepad->hasAnalogTriggers = true;
    gamepad->hasLeftAnalogStick = true;
    gamepad->hasRightAnalogStick = true;
}
