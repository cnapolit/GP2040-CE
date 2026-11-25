#ifndef _NetworkInput_H
#define _NetworkInput_H

#include <stdint.h>

#include "gpaddon.h"

// Network Input: takes the whole pad state from UDP datagrams received by a WIZnet W5500 on SPI, so a computer
// on the LAN can play through whichever input mode the board is in (Satelight's pad bridge).

#ifndef NETWORK_INPUT_ENABLED
#define NETWORK_INPUT_ENABLED 0
#endif

#ifndef NETWORK_INPUT_SPI_BLOCK
#define NETWORK_INPUT_SPI_BLOCK spi0
#endif
#ifndef NETWORK_INPUT_PIN_MISO
#define NETWORK_INPUT_PIN_MISO 16
#endif
#ifndef NETWORK_INPUT_PIN_CS
#define NETWORK_INPUT_PIN_CS 17
#endif
#ifndef NETWORK_INPUT_PIN_SCK
#define NETWORK_INPUT_PIN_SCK 18
#endif
#ifndef NETWORK_INPUT_PIN_MOSI
#define NETWORK_INPUT_PIN_MOSI 19
#endif
#ifndef NETWORK_INPUT_PIN_RST
#define NETWORK_INPUT_PIN_RST 20
#endif

#ifndef NETWORK_INPUT_UDP_PORT
#define NETWORK_INPUT_UDP_PORT 7284
#endif

// With no packet for this long the pad goes neutral, so a sender that dies doesn't hold buttons down.
#ifndef NETWORK_INPUT_TIMEOUT_MS
#define NETWORK_INPUT_TIMEOUT_MS 500
#endif

// With no DHCP answer for this long the board takes a link-local (169.254/16) address.
#ifndef NETWORK_INPUT_DHCP_TIMEOUT_MS
#define NETWORK_INPUT_DHCP_TIMEOUT_MS 15000
#endif

// Keep the W5500 in reset for this long after boot (0: start it at once), so it neither draws power nor makes noise
// while a host enumerates the board.
#ifndef NETWORK_INPUT_START_DELAY_MS
#define NETWORK_INPUT_START_DELAY_MS 0
#endif

// After power-up the board takes a pairing ("SLPK") for this long, even when it has a key already: plugging it in is
// what lets a new client pair. A board with no key takes one at any time.
#ifndef NETWORK_INPUT_PAIR_WINDOW_MS
#define NETWORK_INPUT_PAIR_WINDOW_MS 120000
#endif

#define NetworkInputName "Network Input"

#define NETWORK_INPUT_KEY_SIZE 16

// Pad state datagram, little-endian, 48 bytes. Taken only with the key the board was paired with.
#define NETWORK_INPUT_MAGIC 0x42504C53u // "SLPB"
#define NETWORK_INPUT_VERSION 2

struct __attribute__((packed)) NetworkPadPacket {
    uint32_t magic;
    uint8_t version;
    uint8_t flags;
    uint16_t reserved0;
    uint32_t sequence;
    uint32_t buttons; // GAMEPAD_MASK_B1.. bits
    uint8_t dpad;     // GAMEPAD_MASK_UP/DOWN/LEFT/RIGHT bits
    uint8_t lt;
    uint8_t rt;
    uint8_t reserved1;
    uint16_t lx, ly, rx, ry; // 0..65535, centre 0x7FFF
    uint32_t reserved2;
    uint8_t key[NETWORK_INPUT_KEY_SIZE];
};
static_assert(sizeof(NetworkPadPacket) == 48, "NetworkPadPacket must stay 48 bytes");

// A datagram starting with this magic asks for a NetworkStatusPacket back: how a client finds boards (sent to the
// broadcast address) and sees from the LAN what the console did. Open to anyone; it carries no secret.
#define NETWORK_INPUT_QUERY_MAGIC 0x51504C53u  // "SLPQ"
#define NETWORK_INPUT_STATUS_MAGIC 0x53504C53u // "SLPS"

struct __attribute__((packed)) NetworkStatusPacket {
    uint32_t magic;
    uint8_t version;
    uint8_t inputMode;
    uint8_t usbMounted;
    uint8_t usbSuspended;
    uint32_t uptimeMs;
    uint32_t padPackets;
    uint32_t msSinceLastPad;
    // Switch Pro mode only.
    uint8_t switchReady;
    uint8_t switchInitialized;
    uint8_t switchPlayer;
    uint8_t switchReportCounter;
    uint32_t switchSetReports;
    uint8_t switchLastReportId;
    uint8_t switchLastSubId;
    uint8_t switchLastSubcommand;
    uint8_t vbus; // the USB port's 5 V is present (GPIO24)
    // What the host asked of the USB device since boot (UsbDebugCounters).
    uint32_t usbDeviceDescriptors;
    uint32_t usbConfigDescriptors;
    uint32_t usbHidReportDescriptors;
    uint32_t usbStrings;
    uint32_t usbMounts;
    uint32_t usbUnmounts;
    uint32_t usbSuspends;
    // The RP2350's own view of the USB lines, below TinyUSB.
    uint32_t sieStatus; // SIE_STATUS: VBUS detected, line state, connected, bus reset, ...
    uint32_t sieCtrl;   // SIE_CTRL: pull-up enabled, ...
    uint32_t usbPwr;    // USB_PWR: VBUS detect overrides
    uint8_t tudConnected; // TinyUSB saw a bus reset from the host
    uint8_t reserved[7];
    uint32_t lineStateSamples[4]; // how often each line state was seen: SE0, J, K, SE1
    uint32_t usbEvents[8];        // UsbDebugCounters::events
    uint8_t mac[6];               // the board's Ethernet address, which a client knows it by
    uint8_t paired;               // the board has a key: pad state and commands need it
    uint8_t pairingOpen;          // a pairing would be taken now (no key yet, or within the window after power-up)
    uint8_t reserved2[8];
};
static_assert(sizeof(NetworkStatusPacket) == 144, "NetworkStatusPacket must stay 144 bytes");

// Pairing: "SLPK", four reserved bytes, the key a client chose. Taken while pairingOpen; the board saves it, forgetting
// any key before it, and answers with a NetworkStatusPacket.
#define NETWORK_INPUT_PAIR_MAGIC 0x4B504C53u // "SLPK"

struct __attribute__((packed)) NetworkPairPacket {
    uint32_t magic;
    uint32_t reserved;
    uint8_t key[NETWORK_INPUT_KEY_SIZE];
};
static_assert(sizeof(NetworkPairPacket) == 24, "NetworkPairPacket must stay 24 bytes");

// Commands: "SLPC", a command byte, an argument byte, two reserved bytes, then the key (needed once the board is
// paired).
#define NETWORK_INPUT_COMMAND_MAGIC 0x43504C53u // "SLPC"
#define NETWORK_INPUT_COMMAND_REBOOT 1          // restart, so the host enumerates the board again
#define NETWORK_INPUT_COMMAND_SET_INPUT_MODE 2  // save the argument as the input mode, then restart
#define NETWORK_INPUT_COMMAND_BOOTSEL 3         // restart into the USB bootloader, to be flashed with picotool
#define NETWORK_INPUT_COMMAND_UPDATE 4          // restart into the network loader to take a new firmware (netloader/)

struct __attribute__((packed)) NetworkCommandPacket {
    uint32_t magic;
    uint8_t command;
    uint8_t argument;
    uint16_t reserved;
    uint8_t key[NETWORK_INPUT_KEY_SIZE];
};
static_assert(sizeof(NetworkCommandPacket) == 24, "NetworkCommandPacket must stay 24 bytes");

class NetworkInput : public GPAddon {
public:
    virtual bool available();
    virtual void setup();
    virtual void preprocess() {}
    virtual void process();
    virtual void postprocess(bool sent) {}
    virtual void reinit() {}
    virtual std::string name() { return NetworkInputName; }

private:
    void pollNetwork(uint32_t now);
    void receive(uint32_t now);
    void takeLinkLocalAddress();
    void startChip();

    bool chipStarted = false;
    void sendStatus(uint8_t ip[4], uint16_t port, uint32_t now);
    void pair(const NetworkPairPacket& packet, uint8_t ip[4], uint16_t port, uint32_t now);
    void command(const NetworkCommandPacket& packet);
    bool paired() const;
    bool pairingOpen(uint32_t now) const;
    bool keyMatches(const uint8_t key[NETWORK_INPUT_KEY_SIZE]) const;

    uint32_t padPackets = 0;
    uint32_t lineStateSamples[4] = {0, 0, 0, 0};

    bool chipFound = false;
    bool haveAddress = false;
    bool linkLocal = false;
    uint32_t dhcpStartedAt = 0;
    uint32_t lastDhcpTick = 0;

    bool havePad = false;
    uint32_t lastPacketAt = 0;
    uint32_t lastSequence = 0;
    uint8_t senderIp[4] = {0, 0, 0, 0};
    uint16_t senderPort = 0;
    NetworkPadPacket pad = {};
};

#endif
