#!/usr/bin/env bash
# Art Swap helper - build the payload (in the Lima VM) or the host copy (here).
#
#   helper/tools/build.sh            build/helper/artswapd.elf, built in the VM "ps5"
#   helper/tools/build.sh host       build/helper-host/artswapd, built on this machine
#
# Copyright (C) 2026 Gary Riches
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
target=${1:-payload}

if [[ $target == host ]]; then
    exec make -C "$root/helper" host
fi

if [[ $(uname) == Darwin ]]; then
    exec limactl shell --workdir / ps5 -- bash -lc '. ~/ps5/env.sh; cd "$1"; export PS5_PAYLOAD_SDK="$1/.deps/native/ps5-payload-sdk"; exec make -C helper -j8' _ "$root"
fi
export PS5_PAYLOAD_SDK="$root/.deps/native/ps5-payload-sdk"
exec make -C "$root/helper" -j8
