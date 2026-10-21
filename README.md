# Art Swap

**Change the icon and background of any title on your PS5's home screen, with art
from [SteamGridDB](https://www.steamgriddb.com).**

Art Swap lists every title on the console that has home-screen art, finds each
one on SteamGridDB, shows you that game's art, and writes the one you pick over
the title's own `icon0` or `pic0`/`pic1`. The original is kept beside it as an
`.old` file, so Restore can put it back.

Title `PPSA99318`, version 0.1.0.

> **Status (2026-10-21): built but not yet run on a console.** The helper is
> tested on a Mac against a made-up console and the real SteamGridDB
> (`helper/tests/test_helper.py`). The screens were drawn off the console by
> the preview tool (`tools/preview/`), with the real helper behind them. The
> parts only a console can prove are listed under [Not yet proven](#not-yet-proven).

## What it does

- **Lists your titles**: PS5 and PS4 games from the home screen's own art folder
  (`/user/appmeta`, including `external/`), and homebrew from `/data/homebrew` and
  the `homebrew` folders on USB and extended storage. Each one shows its art as
  it is now and what form that art takes (for example "Icon PNG 512×512, 2 files ·
  Background DDS BC7 3840×2160, 4 files").
- **Finds the game on SteamGridDB** by its name, and remembers the match. If the
  match is wrong, □ lists other matches or searches for any name you type.
- **Icons** come from SteamGridDB's square art, its covers (portrait art,
  shown whole on a blurred fill of itself, or cut to the square) and its icons
  (those 192 pixels or larger).
- **Backgrounds** come from its heroes and its wide art, cut to the
  background's shape (the right stick slides the part kept) or shown whole on a
  blurred fill.
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

The home screen may keep showing the old art until the console restarts.

## Installing

You need the console's homebrew environment running: etaHEN (or kstuff),
ShadowMountPlus, and an **ELF loader on port 9021**. Art Swap starts its helper
through that loader.

1. Copy the `PPSA99318` folder to `/data/homebrew/` (FTP is fine).
2. ShadowMountPlus registers it and Art Swap appears on the home screen.
3. Start it. The first screen says "Starting the helper" for a moment.

## Using it

| Where | Button | What it does |
| --- | --- | --- |
| Titles | ← → (L1 R1 by six) | choose a title |
| | ✕ | change its icon |
| | △ | change its background |
| | □ | restore its original art (when Art Swap changed it) |
| | OPTIONS | settings: API key, look at the console again, helper log |
| | hold OPTIONS | close Art Swap |
| Art | L1 R1 | Square, Covers, Icons (or Heroes, Wide) |
| | D-pad, left stick | choose a picture |
| | △ | Fill (cut to the frame) or Fit (all of it, on a blurred fill) |
| | right stick | slide the part kept, when Fill cuts the picture |
| | ✕ | use it (asks first) |
| | □ | another game, or search by name with the keyboard |
| | ○ | back to the titles |

## The SteamGridDB API key

SteamGridDB's API needs a key. A release build carries one, so Art Swap works
straight away. To use your own (it is free): sign in at steamgriddb.com, open
Preferences, then API. Enter the key in Art Swap's settings, or put it in
`/data/artswap/apikey.txt` over FTP. "Use the built-in key" in the settings
goes back.

The built-in key comes from `SGDB_KEY` in this repository's ignored `.env` at
build time. It never goes into the source.

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
closes. It answers only on 127.0.0.1. Its requests are in
[`helper/protocol.md`](helper/protocol.md).

On the console the helper keeps its files in `/data/artswap/`:

| File | What it is |
| --- | --- |
| `helper.log` | what the helper did, each start (also in Settings, "Helper log") |
| `scan.txt` | every title found, and each art file with its format and size |
| `appmeta-files.txt` | every file in each title's `/user/appmeta` folder |
| `matches.tsv` | the SteamGridDB game chosen for each title |
| `apikey.txt` | your own API key, if you gave one |
| `cache/` | downloaded pictures (trimmed to 192 MB when over 256 MB) |
| `no-appdb` | if present, names are never read from the system's `app.db` |

Names come from the title's `param.json` or `param.sfo`, then from a read-only
copy of the system's `app.db`, and otherwise the title id is shown.

## Not yet proven

These need the console and have not been run there yet:

- a title connecting to 127.0.0.1 (the helper, and the ELF loader on 9021);
- HTTPS from the payload: libSceHttp2 first, then libSceHttp (HTTP/1.1), loaded
  with `dlopen`; a TLS failure is retried once without certificate checks, and
  the log says so;
- the layout of `/user/appmeta` for retail PS5 and PS4 games on this firmware
  (`appmeta-files.txt` records it);
- whether the home screen shows new art without a restart;
- the keyboard dialog (the code is Wind Waker HD's, which works there).

## Building

Everything builds in the Lima VM `ps5` (`~/ps5`: PS5_Vulkan, PS5_PayloadSDK,
the UI kit's fork), with this folder mounted at the same path.

```bash
ps5/tools/vm.sh build                  # dist/PPSA99318/ with eboot.bin and artswapd.elf
helper/tools/build.sh                  # the helper alone: build/helper/artswapd.elf
helper/tools/build.sh host             # the helper for this Mac (libcurl)
python3 helper/tests/test_helper.py    # the helper against a made-up console and SteamGridDB
tools/preview/run.sh                   # the screens with the real helper, drawn in the VM: build/preview/*.png
```

The preview's tour is a script of presses (`tools/preview/preview_main.cpp`
has the default; `tools/preview/run.sh FILE` runs another). Put `SGDB_KEY=...`
in `.env` first: the tests and the preview talk to SteamGridDB.

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
  PS5 layer) is MIT (`LICENSE.md`).
- The **helper** uses [stb](https://github.com/nothings/stb) (public domain or MIT),
  [bcdec](https://github.com/iOrange/bcdec) (MIT or Unlicense),
  [cJSON](https://github.com/DaveGamble/cJSON) (MIT) and [SQLite](https://sqlite.org)
  (public domain); `helper/third_party/SOURCES.txt` pins each. Its BC7 encoder
  follows PS5 RetroArch's `tools/make-title-art.py` (GPL-3.0-or-later). The
  keyboard follows PS5_WindWakerHD's.
- **The art Art Swap offers** is SteamGridDB's, made and uploaded by its
  community; each picture shows its author. Art Swap is not affiliated with
  SteamGridDB, Valve or Sony.
- Art Swap's own icon and background were generated for this project
  (sources in `ps5/art/`).

Art Swap as a whole (the title and the helper) is GPL-3.0-or-later
(`ps5/licenses/GPL-3.0.txt`).
