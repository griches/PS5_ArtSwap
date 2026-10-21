/*
 * Art Swap - change the home-screen art of the titles on the console with
 * SteamGridDB's.
 *
 * This file is compiled inside the namespace the template wraps each program
 * in (ps5/src/wrap.cpp.in); the program itself is kit/program.cpp, compiled
 * as it stands, with the screens (kit/app.cpp), the helper's client
 * (kit/helper_client.cpp) and the keyboard (kit/keyboard.cpp). The helper
 * payload is helper/.
 *
 * Copyright (C) 2026 Gary Riches
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

extern "C" VulkanExampleBase *artswap_make_program();

VulkanExampleBase *createExample()
{
	return artswap_make_program();
}
