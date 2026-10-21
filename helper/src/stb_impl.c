/*
 * Art Swap helper - the single-file libraries' code, compiled once
 * (third_party/SOURCES.txt names each and its licence).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb_image_write.h"

#define STB_DXT_IMPLEMENTATION
#include "../third_party/stb_dxt.h"

#define BCDEC_IMPLEMENTATION
#include "../third_party/bcdec.h"
