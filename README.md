# Art Swap

[![stars](https://img.shields.io/github/stars/griches/PS5_ArtSwap?style=flat)](https://github.com/griches/PS5_ArtSwap/stargazers)
[![release](https://img.shields.io/github/v/release/griches/PS5_ArtSwap)](https://github.com/griches/PS5_ArtSwap/releases/latest)
[![licence](https://img.shields.io/badge/licence-GPL--3.0--or--later-blue)](LICENSE)

**Change the icon and background of any title on your PS5's home screen, with art
from [SteamGridDB](https://www.steamgriddb.com).**

Art Swap lists every title on the console that has home-screen art, finds each
one on SteamGridDB, shows you that game's art, and writes the one you pick over
the title's own `icon0` or `pic0`/`pic1`. The original is kept beside it as an
`.old` file, so Restore can put it back. For homebrew that ShadowMountPlus
installed from a folder, Art Swap can also refresh the home screen so the new
art shows straight away.

Title `PPSA99318`, version 1.1.0. Tested on a PS5 with firmware 13.60.

## What it does

- **Lists your titles**: PS5 and PS4 games from the home screen's own art folder
  (`/user/appmeta`, including `external/`), and homebrew from `/data/homebrew` and
  the `homebrew` folders on USB and extended storage. Each one shows its art as
  it is now and what form that art takes (for example "Icon PNG 512×512, 2 files ·
  Background DDS BC7 3840×2160, 4 files").
- **Finds the game on SteamGridDB** by its name, and remembers the match. If the
  match is wrong, □ lists other matches or searches for any name you type.
- **Icons** come from SteamGridDB's square grids, and **Backgrounds** from its
  heroes (art made for backgrounds).
- **You choose the part kept** (△ in the picker opens the frame page): Fit (all
  of it, on a blurred fill of itself), the left, centre or right of a wide
  picture (top, middle or bottom of a tall one), or a part of your own: the
  left stick or D-pad moves it, R2 and L2 zoom in and out (up to 3×). The page
  shows the whole picture with the part kept lit, and the result beside it.
- **The preview is exact**: what you see under NEW is what the helper will
  write, beside the art there NOW.
- **Writes what the console expects**: each file keeps its format, size,
  channels and DDS layout (PNG, JPEG, or DDS in BC7, BC1, BC3 or RGBA, with the
  same header and number of levels). Every copy is changed together: the home
  screen's copy in `/user/appmeta` and, for homebrew, the one in the title's own
  `sce_sys`. A background changes `pic0` and `pic1` together.
- **Keeps the originals**: before a file is changed the first time, Art Swap
  copies it beside itself as `<name>.old` (`icon0.png.old`, `pic0.dds.old`). Later
  changes never touch an `.old` file, so it is always the art the title came
  with. Restore moves every `.old` file back over its original.
- **Renames homebrew** (L2) that ShadowMountPlus installed from a folder: the
  new name goes into the title's `param.json` (every language), and the
  refresh shows it on the home screen. The original is kept as
  `param.json.old`, and Restore puts it back too.
- **Refreshes the home screen** (R2) for homebrew that ShadowMountPlus installed
  from a folder: see [Refreshing the home screen](#refreshing-the-home-screen).
  For other titles the new art is written, but the home screen keeps its own
  cached copy and may show the old art until it reloads it (a console restart).

## Installing

You need the console's homebrew environment running: etaHEN (or kstuff),
ShadowMountPlus, and an **ELF loader on port 9021**. Art Swap starts its helper
through that loader.

1. Download `PS5_ArtSwap-v1.1.0.zip` from [Releases](https://github.com/griches/PS5_ArtSwap/releases/latest).
2. Copy the `PPSA99318` folder inside it to `/data/homebrew/` (FTP is fine).
3. ShadowMountPlus registers it and Art Swap appears on the home screen.
4. Start it. The first screen says "Starting the helper" for a moment.
5. Add your SteamGridDB API key when it asks (see [below](#the-steamgriddb-api-key)).

## Using it

| Where | Button | What it does |
| --- | --- | --- |
| Titles | ← → (L1 R1 by six) | choose a title |
| | ✕ | change its icon |
| | △ | change its background |
| | □ | restore its original art and name (when Art Swap changed them) |
| | R2 | refresh the home screen (asks first) |
| | L2 | rename it, then refresh (homebrew from a folder) |
| | OPTIONS | settings: API key, look at the console again, helper log |
| | hold OPTIONS | close Art Swap |
| Art | L1 R1 | the Icons and Backgrounds tabs |
| | D-pad, left stick | choose a picture |
| | △ | the frame page: which part of the picture is kept |
| | right stick | slide the part kept, when Fill cuts the picture |
| | ✕ | use it (asks first) |
| | □ | another game, or search by name with the keyboard |
| | ○ | back to the titles |
| Frame | L1 R1 | Fit, Left, Centre, Right (or Top, Middle, Bottom), Custom |
| | left stick, D-pad | move the part kept |
| | R2 L2 | zoom in, zoom out |
| | ✕ | use it (asks first) |
| | ○ | back to the pictures |

## Refreshing the home screen

The home screen keeps its own copy of each title's art, so new art shows only
once the title is installed again. For homebrew, that is what deleting the
title and letting ShadowMountPlus add it back does, and Art Swap does it for
you: R2 on a title opens a pop-up that says what will happen, and ✕ starts it.
After a rename (L2) the same pop-up follows, since a name shows the same way.

1. Art Swap notes every file in the game's folder (each one's path and size)
   and copies aside the console's saved data of the title (every user's
   `savedata_prospero/<id>` and `savedata/<id>`) and Art Swap's originals in the
   home screen's copy, to `/data/artswap/saves/<id>-<time>/`. If it cannot read
   the whole folder or make the whole copy, it stops and changes nothing.
2. Art Swap closes: ShadowMountPlus will not take a title away while any game
   is open. The helper carries on from the home screen.
3. The helper asks ShadowMountPlus (its API on 127.0.0.1:10101) to uninstall the
   title. That removes only the home screen's entry; the game's folder, with
   its own saves and settings, stays where it is. A scan then adds the title
   back with its new art, and the helper waits until the install has finished.
4. The helper checks every file of the folder against its notes, and puts back
   any saved data or original that went missing. The next time Art Swap starts
   it says how the refresh went.

Nothing is deleted. If the game's folder had changed, the refresh says so
rather than calling itself done (the folder itself is not copied: the
uninstall never touches it). The copy from the last refresh that went well is
kept for each title, and the copy from any that did not.

A refresh needs **ShadowMountPlus 1.7 or later**, running, and a title it
installed **from a folder**. Art Swap never refreshes a title installed from a
package (taking its tile away would delete the game) or from an image (its art
is inside the image, which Art Swap does not change); the pop-up says why.

## The SteamGridDB API key

SteamGridDB's API needs a key, and each person registers their own: it is free,
and SteamGridDB's terms do not cover sharing one. No key is built into Art Swap.

When Art Swap has no key it says so: on the first start, when ✕ or △ would need
SteamGridDB, and at the top of the screen. It explains what the key is and how
to get one (steamgriddb.com, sign in, Preferences, API), and offers two ways to
add it:

- **From your phone**: the TV shows a QR code, the console's address and a
  four-digit code. The phone opens a small page served by the helper on port
  9118, takes the key, checks it with SteamGridDB and saves it; the TV notices
  and carries on. The page exists only while that screen is open (ten minutes
  at most), needs the code, and closes after ten wrong codes.
- **With the console's keyboard**.

Settings (OPTIONS) shows whether a key is in, with its last four characters,
and can add, replace or remove it, refresh the game list, or show the helper's
log. The key is kept in `/data/artswap/apikey.txt`, which can also be copied
over FTP. Without a key, Art Swap still lists your games and restores original
art.

## How it works

A title runs in a sandbox and sees only its own folder, so it cannot read or
write `/user/appmeta`. Art Swap is therefore two programs:

```text
  Art Swap (the title, PPSA99318)           artswapd (the helper payload)
  the screens, drawn with the UI kit  <-->  reads and writes the art, talks to
  helper_client: 4 connections,      TCP    SteamGridDB, makes the pictures
  answers on the drawing thread   127.0.0.1:9117
          |
          '-- at start, if no helper answers: artswapd.elf --> ELF loader (127.0.0.1:9021)
```

The title carries the helper (`artswapd.elf` in its folder) and hands it to the
ELF loader when no helper answers. A helper of another version is asked to
leave and replaced. The helper leaves by itself 90 seconds after Art Swap
closes. It answers only on 127.0.0.1 (port 9117, or 9127 or 9137 when a helper
that crashed still holds one), apart from the key page above while it is open. Its requests are in
[`helper/protocol.md`](helper/protocol.md).

On the console the helper keeps its files in `/data/artswap/`:

| File | What it is |
| --- | --- |
| `helper.log` | what the helper did, each start (also in Settings, "Helper log") |
| `scan.txt` | every title found, and each art file with its format and size |
| `appmeta-files.txt` | every file in each title's `/user/appmeta` folder |
| `matches.tsv` | the SteamGridDB game chosen for each title |
| `refresh.txt` | how the last home-screen refresh ended |
| `saves/` | what each refresh copied aside (see above) |
| `apikey.txt` | your SteamGridDB API key |
| `cache/` | downloaded pictures (trimmed to 192 MB when over 256 MB) |
| `no-appdb` | if present, names are never read from the system's `app.db` |

Names come from the title's `param.json` or `param.sfo`, then from a read-only
copy of the system's `app.db`, and otherwise the title id is shown.

## Tested on the console

On a PS5 with firmware 13.60, ShadowMountPlus 1.7beta3: the title reaches the
helper on 127.0.0.1 and starts it through the ELF loader; the scan (20 titles
in 40-240 ms; `/user/appmeta/<id>/` holds `icon0.png`, `pic0.dds`, `pic1.dds`,
`param.json` for a homebrew title); HTTPS to SteamGridDB's API and CDN
(helper/tests/nettest.c); the key page from another machine on the network;
changing and restoring icons and backgrounds; and the home-screen refresh (new
art on the tile within seconds, the game's saves identical afterwards, file
for file).

What the console taught (helper/tests/tlstest.c, procs.c):

- A payload does not get the console's trusted roots: every HTTPS request
  fails with `0x8095F00C` (TLS `0x8095F00A`, verify error `0x20`, unknown CA).
  The helper's certificate callback accepts that one result and refuses any
  other (`http_ps5.c`). `sceHttpsDisableOption` crashes the process; HTTP/2
  fails the same way and is not used.
- A payload that crashes is stopped, not ended: it stays under the ELF loader,
  traced by it (`PT_ATTACH` gives `EALREADY`, SIGKILL leaves it), and keeps its
  sockets until the console restarts. So the helper takes the first free of
  9117, 9127, 9137 (and the key page of 9118, 9128, 9138), the title asks each
  for HELLO with a three-second limit, and starts the helper again when it has
  gone.
- ShadowMountPlus refuses to uninstall (`EBUSY`) while any game is in the
  foreground, Art Swap included, so the refresh finishes from the home screen.
  Its game list says `installed` before the install has copied anything; the
  helper waits for the copies' change times instead.
- Folders a game writes (its saves, mode 0700) can be read only as the game's
  user, not even as root: the helper switches to the folder's owner to read
  them.

## Building

Everything builds in the Lima VM `ps5` (`~/ps5`: PS5_Vulkan, PS5_PayloadSDK,
the UI kit's fork), with this folder mounted at the same path.

```bash
ps5/tools/vm.sh build                  # dist/PPSA99318/ with eboot.bin and artswapd.elf
helper/tools/build.sh                  # the helper alone: build/helper/artswapd.elf
helper/tools/build.sh host             # the helper for this Mac (libcurl)
python3 helper/tests/test_helper.py    # the helper against a made-up console and SteamGridDB, the key page too
tools/preview/run.sh                   # the screens with the real helper, drawn in the VM: build/preview/*.png
NOKEY=1 tools/preview/run.sh FILE      # ... as someone who has not added a key yet
```

The preview's tour is a script of presses (`tools/preview/preview_main.cpp`
has the default; `tools/preview/run.sh FILE` runs another). Put `SGDB_KEY=...`
in `.env` first: the tests and the preview talk to SteamGridDB, and put that key
into their made-up console's `apikey.txt`. It never goes into a build.

The title is made from the [PS5 Vulkan Template](https://github.com/mihawk-99/PS5_VulkanTemplate)
at `b577e950`, with its UI module. Art Swap's code is in `examples/artswap/kit/`
(the screens `app.cpp`, the helper's client, the keyboard, the program) and
`helper/`.

## Credits and licences

- The **UI kit** is BlackBearReloaded's [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui),
  drawn with Vulkan through PS5_VKHomebrewUI, GPL-3.0-or-later. Art Swap's
  screens follow its Aurora Shelf design. Its fonts are OFL-1.1 and Bitstream
  Vera (`ps5/ASSETS.md`).
- The **foundation** (Sascha Willems' Vulkan example base and the template's
  PS5 layer) is MIT (`ps5/licenses/MIT.txt`).
- The **helper** uses [stb](https://github.com/nothings/stb) (public domain or MIT),
  [bcdec](https://github.com/iOrange/bcdec) (MIT or Unlicense),
  [cJSON](https://github.com/DaveGamble/cJSON) (MIT) and [SQLite](https://sqlite.org)
  (public domain); `helper/third_party/SOURCES.txt` pins each. Its BC7 encoder
  follows PS5 RetroArch's `tools/make-title-art.py` (GPL-3.0-or-later). The
  keyboard follows PS5_WindWakerHD's.
- The QR code is drawn with [Nayuki's QR Code generator](https://github.com/nayuki/QR-Code-generator)
  (MIT), in `examples/artswap/kit/qrcodegen.*`.
- **The art Art Swap offers** is SteamGridDB's, made and uploaded by its
  community; each picture shows its author. Art Swap is not affiliated with
  SteamGridDB, Valve or Sony.
- Art Swap's own icon and background were generated for this project
  (sources in `ps5/art/`).

Art Swap as a whole (the title and the helper) is GPL-3.0-or-later
([`LICENSE`](LICENSE)).
