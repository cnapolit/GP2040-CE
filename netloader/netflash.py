"""Flash GP2040-CE onto a pad bridge over the network, through its network loader.

Usage: python3 netflash.py HOST FIRMWARE.bin [KEY]

KEY is the 32 hex digits a client paired the board with (or $NETWORK_INPUT_KEY); a board nobody paired needs none.

The firmware must be the .bin of a build linked behind the loader (a board config with pico_flash_region.ld). The
board's running firmware is asked to restart into the loader (UDP command 4 on port 7284), the image goes over TCP
port 7285, and the loader starts it once the CRC checks out.
"""
import os
import socket
import struct
import sys
import time
import zlib

PAD_PORT = 7284
LOADER_PORT = 7285
APP_MAX = 0x1F8000 - 0x20000


def status(host):
    """Returns ("app", raw) or ("loader", raw) for whatever answers, or (None, None)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.5)
    try:
        s.sendto(struct.pack("<I", 0x51504C53), (host, PAD_PORT))
        data, _ = s.recvfrom(256)
    except OSError:
        return None, None
    finally:
        s.close()
    magic = struct.unpack("<I", data[:4])[0]
    return {0x53504C53: "app", 0x50504C53: "app", 0x4C504C53: "loader"}.get(magic), data


def wait_for(host, want, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        kind, _ = status(host)
        if kind == want:
            return True
        time.sleep(0.5)
    return False


def main():
    host, path = sys.argv[1], sys.argv[2]
    key = bytes.fromhex(sys.argv[3] if len(sys.argv) > 3 else os.environ.get("NETWORK_INPUT_KEY", "0" * 32))
    if len(key) != 16:
        sys.exit("the key is 32 hex digits")
    image = open(path, "rb").read()
    if not 4096 < len(image) <= APP_MAX:
        sys.exit(f"{path}: {len(image)} bytes doesn't fit the firmware region ({APP_MAX} bytes)")
    vectors = struct.unpack("<II", image[:8])
    if not (0x20000000 <= vectors[0] <= 0x20082000 and 0x10020000 <= vectors[1] < 0x101F8000):
        sys.exit(f"{path}: not linked behind the loader (stack 0x{vectors[0]:08x}, reset 0x{vectors[1]:08x})")

    kind, _ = status(host)
    if kind == "app":
        print("asking the firmware to restart into the loader")
        u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        u.sendto(struct.pack("<IBBH16s", 0x43504C53, 4, 0, 0, key), (host, PAD_PORT))
        u.close()
        time.sleep(1)
        if not wait_for(host, "loader", 30):
            sys.exit("the loader never answered (a paired board needs its key)")
    elif kind == "loader":
        print("already in the loader")
    else:
        sys.exit(f"nothing answers at {host}")

    crc = zlib.crc32(image) & 0xFFFFFFFF
    print(f"sending {len(image)} bytes, crc 0x{crc:08x}")
    t0 = time.monotonic()
    c = socket.create_connection((host, LOADER_PORT), timeout=60)
    c.sendall(struct.pack("<IIII", 0x55504C53, len(image), crc, 0))
    c.sendall(image)
    reply = b""
    while not (reply.endswith(b"OK\n") or b"ERR" in reply and reply.endswith(b"\n")):
        chunk = c.recv(64)
        if not chunk:
            break
        reply += chunk
    c.close()
    print(f"loader: {reply.decode(errors='replace').strip()} ({time.monotonic() - t0:.1f} s)")
    if not reply.endswith(b"OK\n"):
        sys.exit(1)

    if wait_for(host, "app", 30):
        print("new firmware is running")
    else:
        sys.exit("the new firmware didn't answer within 30 s")


if __name__ == "__main__":
    main()
