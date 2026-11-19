#!/usr/bin/env python3
"""Push a .nro to hbmenu's netloader, the way nxlink does.

The container this repo builds in cannot reach the LAN on macOS, so this runs on the host and speaks
the same protocol as nxlink (see nx-hbmenu common/netloader.c):

    u32 name length, name
    u32 file length
    u32 response (0 = ok)
    repeated: u32 chunk length, zlib bytes   until the file length is reached
    u32 response (0 = ok)
    u32 argument length, arguments

    python3 nxlink.py <switch ip> <file.nro> [args...]
"""
import socket
import struct
import time
import sys
import zlib

PORT = 28280
STDOUT_PORT = 28281
IDLE = 5          # stop after this long with nothing arriving
MAX_LISTEN = 20   # and never hold the terminal longer than this, heartbeat or not   # the app connects back here after nxlinkStdio()
CHUNK = 8192            # input slice; compressed output stays under the console's 16 KB inflate buffer
MAX_CHUNK = 16384        # hbmenu rejects a chunk larger than ZLIB_CHUNK (16 KB)


def recv_exact(sock: socket.socket, size: int) -> bytes:
    data = b""
    while len(data) < size:
        part = sock.recv(size - len(data))
        if not part:
            raise ConnectionError("the console closed the connection")
        data += part
    return data


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    host, path = sys.argv[1], sys.argv[2]
    args = " ".join(sys.argv[3:])

    payload = open(path, "rb").read()
    name = path.rsplit("/", 1)[-1].encode()

    # Listen before pushing: the app calls nxlinkStdio() and connects back to us as it starts.
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("0.0.0.0", STDOUT_PORT))
    listener.listen(1)
    listener.settimeout(3)   # a push should feel instant; no log is a normal outcome

    with socket.create_connection((host, PORT), timeout=10) as sock:
        print(f"connected; sending {len(payload)} bytes as {name.decode()}", flush=True)
        sock.sendall(struct.pack("<I", len(name)) + name)
        sock.sendall(struct.pack("<I", len(payload)))
        print("name and size sent; waiting for the console to open the file", flush=True)

        (response,) = struct.unpack("<i", recv_exact(sock, 4))
        if response != 0:
            print(f"the console refused the transfer (response {response})")
            return 1

        # hbmenu inflates a stream of length-prefixed zlib chunks.
        compressor = zlib.compressobj(6)
        sent = 0
        while sent < len(payload):
            piece = payload[sent:sent + CHUNK]
            sent += len(piece)
            compressed = compressor.compress(piece)
            for start in range(0, len(compressed), MAX_CHUNK):
                piece_out = compressed[start:start + MAX_CHUNK]
                sock.sendall(struct.pack("<I", len(piece_out)) + piece_out)
        tail = compressor.flush()
        for start in range(0, len(tail), MAX_CHUNK):
            piece_out = tail[start:start + MAX_CHUNK]
            sock.sendall(struct.pack("<I", len(piece_out)) + piece_out)

        print("data sent; waiting for the console to confirm the file", flush=True)
        (response,) = struct.unpack("<i", recv_exact(sock, 4))
        if response != 0:
            print(f"the console did not accept the file (response {response})")
            return 1

        encoded = args.encode()
        sock.sendall(struct.pack("<I", len(encoded)) + encoded)
        print(f"sent {len(payload)} bytes as {name.decode()}; hbmenu is starting it", flush=True)

    try:
        app, addr = listener.accept()
        print(f"--- app output (from {addr[0]}) ---", flush=True)
        # The app keeps this socket open for its whole run, so idle time, not EOF, ends the read.
        app.settimeout(IDLE)
        deadline = time.time() + MAX_LISTEN
        with app:
            while True:
                if time.time() >= deadline:
                    print(f"(stopped listening after {MAX_LISTEN}s; the app keeps running on the console)", flush=True)
                    break
                try:
                    chunk = app.recv(4096)
                except socket.timeout:
                    print(f"(no output for {IDLE}s; the app is still running on the console)", flush=True)
                    break
                if not chunk:
                    break
                sys.stdout.write(chunk.decode("utf-8", "replace"))
                sys.stdout.flush()
    except socket.timeout:
        print("(no log stream this run; continuing)", flush=True)
    finally:
        listener.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
