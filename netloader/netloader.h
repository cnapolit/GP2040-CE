// Shared between the network loader and the firmware it starts (the Network Input addon).
#ifndef NETLOADER_H
#define NETLOADER_H

// Flash layout on a 2 MB board:
//   0x000000-0x01FFFF  the loader (written only over USB)
//   0x020000-0x1F7FFF  the firmware, replaceable over the network
//   0x1F8000-0x1FFFFF  GP2040-CE's settings (FlashPROM), never touched
#define NETLOADER_APP_OFFSET 0x00020000u
#define NETLOADER_APP_END 0x001F8000u
#define NETLOADER_APP_LENGTH (NETLOADER_APP_END - NETLOADER_APP_OFFSET)

// The firmware sets this in watchdog scratch 2 and reboots to stay in the loader for an update.
#define NETLOADER_SCRATCH_INDEX 2
#define NETLOADER_UPDATE_MAGIC 0x55504454u // "TDPU"

// The loader takes an image on this TCP port: a 16-byte header ("SLPU", length, CRC-32 of the image, 0), then the
// image itself (the firmware's .bin, linked at NETLOADER_APP_OFFSET). It answers "OK" or "ERR <why>" and, on OK,
// starts the new firmware.
#define NETLOADER_TCP_PORT 7285
#define NETLOADER_HEADER_MAGIC 0x55504C53u // "SLPU"

// While in the loader, a UDP "SLPQ" on the pad port is answered with "SLPL" and the loader's state.
#define NETLOADER_STATUS_MAGIC 0x4C504C53u // "SLPL"

#endif
