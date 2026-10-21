#!/usr/bin/env python3
# Art Swap helper - the built-in SteamGridDB key, as a masked C string.
#
#   key-header.py ENV_FILE OUT_HEADER
#
# Reads SGDB_KEY from ENV_FILE (the project's ignored .env) and writes
# ARTSWAP_BUILTIN_KEY, each byte masked as config.c unmasks it, so the key is
# not a plain string in the payload. No key: an empty string. The header is
# rewritten only when it changes, so make does not rebuild for nothing.
#
# Copyright (C) 2026 Gary Riches
# SPDX-License-Identifier: GPL-3.0-or-later
import re
import sys
from pathlib import Path


def main() -> int:
    env, out = Path(sys.argv[1]), Path(sys.argv[2])
    key = ""
    if env.exists():
        for line in env.read_text().splitlines():
            m = re.match(r"\s*SGDB_KEY\s*=\s*['\"]?([A-Za-z0-9]+)", line)
            if m:
                key = m.group(1)
    masked = "".join(f"\\x{ord(c) ^ ((0x5a + i * 7) & 0xff):02x}" for i, c in enumerate(key))
    text = ("/* Written by helper/tools/key-header.py: the built-in SteamGridDB key, masked. Never commit. */\n"
            f'#define ARTSWAP_BUILTIN_KEY "{masked}"\n')
    out.parent.mkdir(parents=True, exist_ok=True)
    if not out.exists() or out.read_text() != text:
        out.write_text(text)
    print(f"built-in key: {'yes' if key else 'none'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
