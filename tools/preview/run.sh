#!/usr/bin/env bash
# Art Swap - the screens drawn off the console, in the Lima VM.
#
#   tools/preview/run.sh [SCRIPT]       pictures in build/preview/
#
# Builds the helper for Linux and the preview tool (the kit, the screens,
# tools/preview/preview_main.cpp), lays out a made-up console
# (helper/tests/test_helper.py's), starts the helper on it, and runs the
# preview's script of presses against it. The helper talks to SteamGridDB, so
# this needs the network and a key: SGDB_KEY in .env goes into the made-up
# console's /data/artswap/apikey.txt (NOKEY=1 leaves it out, to see Art Swap
# without one). SEED=folder is copied over the made-up console first (for
# example data/artswap/pending.txt, to start with changes waiting).
#
# Copyright (C) 2026 Gary Riches
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
if [[ $(uname) == Darwin ]]; then
    exec limactl shell --workdir / ps5 -- env NOKEY="${NOKEY:-}" SEED="${SEED:-}" bash -lc 'cd "$1"; shift; exec bash tools/preview/run.sh "$@"' _ "$root" "$@"
fi
script=${1:-}
work=$HOME/ps5/build/artswap-preview
out=$root/build/preview
kit=$root/.deps/hui
mkdir -p "$work/obj" "$out"

make -s -C "$root/helper" host HOST_OUT="$work/helper" >/dev/null

# the preview: the kit without its console layer, entry point and OpenGL backend
sources=("$root/tools/preview/preview_main.cpp" "$root/tools/preview/platform_host.cpp"
    "$root/examples/artswap/kit/app.cpp" "$root/examples/artswap/kit/helper_client.cpp"
    "$root/examples/artswap/kit/keyboard.cpp" "$root/examples/artswap/kit/qrcodegen.cpp"
    "$root/examples/artswap/kit/settings.cpp" "$root/examples/artswap/kit/music.cpp")
while IFS= read -r -d '' source; do
    sources+=("$source")
done < <(find "$kit/src" -type f \( -name '*.cpp' -o -name '*.c' \) ! -path "$kit/src/platform/*" \
    ! -path "$kit/src/runtime/*" ! -name main.cpp ! -name 'gl_*.cpp' ! -path "$kit/src/gfx/backdrop.cpp" \
    ! -path "$kit/src/gfx/canvas.cpp" -print0 | sort -z)
objects=()
pids=()
for source in "${sources[@]}"; do
    object="$work/obj/$(echo "${source#"$root/"}" | tr / _).o"
    objects+=("$object")
    if [[ ! -f $object || $source -nt $object || $root/examples/artswap/kit/app.hpp -nt $object || $root/examples/artswap/kit/title.hpp -nt $object ||
            $root/examples/artswap/kit/helper_client.hpp -nt $object ]]; then
        if [[ $source == *.c ]]; then
            clang -std=c11 -O1 -w -I"$kit/src" -c "$source" -o "$object" &
        else
            clang++ -std=c++20 -O1 -Wall -Wno-unused-parameter -I"$kit/src" -I"$root/examples/artswap/kit" \
                -I"$root/tools/preview" -I"$root/helper/third_party" -c "$source" -o "$object" &
        fi
        pids+=($!)
        if (( ${#pids[@]} >= 8 )); then
            wait "${pids[0]}"
            pids=("${pids[@]:1}")
        fi
    fi
done
for pid in "${pids[@]}"; do wait "$pid"; done
clang++ "${objects[@]}" -lvulkan -lpthread -o "$work/preview"

# a made-up console, and the helper on it
python3 - "$work/console-root" "$root/helper/tests" <<'PY'
import shutil, sys
from pathlib import Path
sys.path.insert(0, sys.argv[2])
import test_helper
base = Path(sys.argv[1])
if base.exists():
    shutil.rmtree(base)
test_helper.make_console(base)
(base / "data/artswap/music").mkdir(parents=True, exist_ok=True)
test_helper.make_song(base / "data/artswap/music/Theme.wav", 45)
PY
[[ -z ${SEED:-} ]] || cp -R "$SEED"/. "$work/console-root/"
key=$(sed -n 's/^SGDB_KEY=//p' "$root/.env" 2>/dev/null || true)
[[ -n ${NOKEY:-} || -z $key ]] || printf '%s\n' "$key" > "$work/console-root/data/artswap/apikey.txt"
port=19217
# ShadowMountPlus's API, as helper/tests/fake_smp.py plays it, for the home-screen refresh
python3 "$root/helper/tests/fake_smp.py" "$work/console-root" 19201 &
smp=$!
ARTSWAP_ROOT="$work/console-root" ARTSWAP_PORT=$port ARTSWAP_LAN=192.168.0.190 ARTSWAP_SMP_PORT=19201 \
    "$work/helper/artswapd" > "$work/helper.out" 2>&1 &
helper=$!
trap 'kill $helper $smp 2>/dev/null || true' EXIT
sleep 1
rm -f "$out"/*.png
"$work/preview" "$root/dist/PPSA99318/assets/hui" "$out" "$port" ${script:+"$script"}
echo "==> $out"
