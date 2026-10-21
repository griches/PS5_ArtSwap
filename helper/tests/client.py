#!/usr/bin/env python3
# Art Swap helper - a client for its protocol (helper/protocol.md), for tests
# and for poking a helper by hand.
#
#   client.py [--host H] [--port P] REQUEST [FIELD...]     prints text, or saves a picture as out.png
#
# Copyright (C) 2026 Gary Riches
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import socket
import sys


class HelperError(Exception):
    pass


class Helper:
    def __init__(self, host="127.0.0.1", port=9117, timeout=120.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.file = self.sock.makefile("rb")

    def close(self):
        self.file.close()
        self.sock.close()

    def ask(self, *fields):
        """One request: text (str), or (width, height, rgba bytes) for a picture."""
        self.sock.sendall(("\t".join(str(f) for f in fields) + "\n").encode())
        head = self.file.readline().decode().rstrip("\n")
        if head.startswith("ERR "):
            raise HelperError(head[4:])
        if not head.startswith("OK "):
            raise HelperError(f"bad answer: {head!r}")
        length, width, height = (int(v) for v in head[3:].split())
        body = self.file.read(length) if length else b""
        if len(body) != length:
            raise HelperError("answer cut short")
        if width or height:
            return width, height, body
        return body.decode()

    def rows(self, *fields):
        return [line.split("\t") for line in self.ask(*fields).splitlines() if line]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9117)
    parser.add_argument("request", nargs="+")
    args = parser.parse_args()
    helper = Helper(args.host, args.port)
    try:
        answer = helper.ask(*args.request)
    except HelperError as e:
        print(f"ERR {e}", file=sys.stderr)
        return 1
    if isinstance(answer, tuple):
        from PIL import Image
        w, h, rgba = answer
        Image.frombytes("RGBA", (w, h), rgba).save("out.png")
        print(f"{w}x{h} picture saved as out.png")
    else:
        print(answer, end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
