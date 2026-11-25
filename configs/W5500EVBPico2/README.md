# WIZnet W5500-EVB-Pico2

An RP2350 board with a WIZnet W5500 Ethernet chip, used as a network pad bridge: it plugs into a console's USB port
and acts as a controller in whichever input mode is selected (Switch Pro by default), taking the whole pad state from
UDP datagrams on port 7284 via the Network Input addon. It has no buttons of its own.

- W5500 on SPI0: MISO GPIO16, CS GPIO17, SCK GPIO18, MOSI GPIO19, RST GPIO20, INT GPIO21.
- Address from DHCP, or a link-local 169.254.x.y address when nothing answers within 15 s (e.g. cabled straight to a
  computer).
- Pad state packet: see `NetworkPadPacket` in `headers/addons/network_input.h`. With no packet for 500 ms the pad goes
  neutral.
- Pairing: a client (Satelight's TV client, its `providers/bridge`) sends `"SLPK"` with a 16-byte key it made up. The board
  takes it while it has no key, or within two minutes of powering up (plugging it into the console again), and keeps it in
  its settings (`Config.networkInputKey`, never shown by the web configurator). From then on pad state and commands are
  taken only with that key; the status query stays open to anyone, for discovery by broadcast.

Build with `GP2040_BOARDCONFIG=W5500EVBPico2`.

## Network loader and flashing over Ethernet

The board's 2 MB of flash is split three ways:

| Flash range | Holds |
|---|---|
| 0x000000–0x01FFFF | the network loader (`netloader/`), written only over USB |
| 0x020000–0x1F7FFF | GP2040-CE, linked there by `pico_flash_region.ld` in this folder |
| 0x1F8000–0x1FFFFF | GP2040-CE's settings |

The loader starts GP2040-CE unless it was asked for an update or finds no valid firmware. Then it brings up the W5500
and takes an image over TCP port 7285, writing its first sector last so a cut-off update leaves the board in the loader
rather than bricked.

- Install once over USB (BOOTSEL): `picotool load build-netloader/netloader.uf2` then the GP2040-CE `.uf2`.
- Then flash over the network: `python3 netloader/netflash.py <board IP> build-w5500/GP2040-CE_<version>_W5500EVBPico2.bin`.
- Build the loader with `cmake -S netloader -B build-netloader -G Ninja && ninja -C build-netloader`.

UDP commands on port 7284 (`"SLPC"`, command byte, argument byte, two reserved bytes, the key): 1 reboot, 2 set input
mode and reboot, 3 restart into BOOTSEL, 4 restart into the network loader. `netflash.py` takes the key as its third
argument (or `$NETWORK_INPUT_KEY`); a board nobody paired needs none. `"SLPQ"` gets a status reply (`"SLPS"` from GP2040-CE, `"SLPL"` from the
loader).
