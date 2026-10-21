#!/usr/bin/env bash
# Art Swap helper - build the payload (in the Lima VM) or the host copy (here).
#
#   helper/tools/build.sh            build/helper/artswapd.elf, built in the VM "ps5"
#   helper/tools/build.sh host       build/helper-host/artswapd, built on this machine
#
# The built-in SteamGridDB key comes from SGDB_KEY in the project's .env (never
# committed); without one the helper has no key of its own and asks for one.
#
# Copyright (C) 2026 Gary Riches
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
target=${1:-payload}

python3 "$root/helper/tools/key-header.py" "$root/.env" "$root/build/helper/builtin_key.h"

if [[ $target == host ]]; then
    exec make -C "$root/helper" host
fi

if [[ $(uname) == Darwin ]]; then
    exec limactl shell --workdir / ps5 -- bash -lc '. ~/ps5/env.sh; cd "$1"; export PS5_PAYLOAD_SDK="$1/.deps/native/ps5-payload-sdk"; exec make -C helper -j8' _ "$root"
fi
export PS5_PAYLOAD_SDK="$root/.deps/native/ps5-payload-sdk"
exec make -C "$root/helper" -j8
