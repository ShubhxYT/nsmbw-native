#!/usr/bin/env python3
"""Mirror the extracted game onto the console's SD card over DBI's FTP server.

DBI (installed as an .nsp on the console) serves the SD card on port 5000 when its FTP mode is
started. The runtime reads the disc image from sdmc:/switch/aaa-nsmbw/game, so this copies the local
extraction there once and skips files that already match in size.

    python3 ftp_game.py <switch ip> [local game dir] [remote dir]
"""
import ftplib
import os
import sys

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.111"
LOCAL = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser("~/wii-port/disc")
REMOTE = sys.argv[3] if len(sys.argv) > 3 else "sdmc:/switch/aaa-nsmbw/game"


def ensure_dir(ftp: ftplib.FTP, path: str) -> None:
    try:
        ftp.cwd(path)
        return
    except ftplib.error_perm:
        pass
    ftp.mkd(path)
    ftp.cwd(path)


def main() -> int:
    ftp = ftplib.FTP()
    ftp.connect(HOST, 5000, timeout=15)
    ftp.login()  # DBI's FTP has no credentials
    print(f"connected to {HOST}:5000; mirroring {LOCAL} -> {REMOTE}", flush=True)

    ensure_dir(ftp, REMOTE)
    uploaded = 0
    skipped = 0
    for root, _dirs, files in os.walk(LOCAL):
        relative = os.path.relpath(root, LOCAL)
        if relative != ".":
            ensure_dir(ftp, relative.replace(os.sep, "/"))
        for name in files:
            local_path = os.path.join(root, name)
            size = os.path.getsize(local_path)
            try:
                if ftp.size(name) == size:
                    skipped += 1
                    continue
            except ftplib.error_perm:
                pass
            with open(local_path, "rb") as handle:
                ftp.storbinary(f"STOR {name}", handle, blocksize=1 << 16)
            uploaded += 1
            print(f"  {os.path.join(relative, name)} ({size} bytes)", flush=True)
        if relative != ".":
            ftp.cwd(REMOTE)

    ftp.cwd("/")
    ftp.quit()
    print(f"done: {uploaded} uploaded, {skipped} already present")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
