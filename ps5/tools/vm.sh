#!/usr/bin/env bash
# Art Swap - run a build tool inside the Linux VM, from macOS.
#
#   ps5/tools/vm.sh build             ps5/tools/build.sh (the title, into dist/PPSA99317/)
#   ps5/tools/vm.sh TOOL [arguments]  any other ps5/tools/TOOL.sh
#
# The stack (PS5_Vulkan with its RADV archive, PS5_PayloadSDK, the UI kit's
# fork) lives in the Lima VM "ps5" under ~/ps5, built with LLVM 22
# (~/ps5/env.sh); this folder is mounted there at the same path. build/ps5 is a
# link to the VM's own disk (~/ps5/build/PS5_ArtSwap/ps5): the shared folder is
# several times slower. The console is reached from the Mac, not from the VM.
#
# SPDX-License-Identifier: MIT
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
tool=${1:?usage: vm.sh build|TOOL [arguments]}
shift
exec limactl shell --workdir / ps5 -- bash -lc '
    . ~/ps5/env.sh
    export PS5_VULKAN_DIR=~/ps5/PS5_Vulkan PS5_PAYLOAD_SDK_FORK=~/ps5/PS5_PayloadSDK
    export PS5_VKHOMEBREWUI=~/ps5/PS5_VulkanTemplate/.deps/PS5_VKHomebrewUI.git PS5_MESA_DIR=~/ps5/PS5_Mesa
    cd "$1"
    if [[ ! -e build/ps5 ]]; then
        mkdir -p build ~/ps5/build/PS5_ArtSwap/ps5
        ln -s ~/ps5/build/PS5_ArtSwap/ps5 build/ps5
    fi
    tool=$2
    shift 2
    exec bash "ps5/tools/$tool.sh" "$@"' _ "$root" "$tool" "$@"
