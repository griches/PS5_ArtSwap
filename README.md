# Art Swap

[![stars](https://img.shields.io/github/stars/griches/PS5_ArtSwap?style=flat)](https://github.com/griches/PS5_ArtSwap/stargazers)
[![release](https://img.shields.io/github/v/release/griches/PS5_ArtSwap)](https://github.com/griches/PS5_ArtSwap/releases/latest)
[![licence](https://img.shields.io/badge/licence-GPL--3.0--or--later-blue)](LICENSE)

**Change the icon, background and logo of any title on your PS5's home screen, with
art from [SteamGridDB](https://www.steamgriddb.com).**

Art Swap lists every title on the console that has home-screen art, finds each
one on SteamGridDB, shows you that game's art, and writes the one you pick over
the title's own `icon0` or `pic0`/`pic1`. The original is kept beside it as an
`.old` file, so Restore can put it back. **Apply changes** (R2) then restarts
the home screen, and every change shows at once, whatever installed the game:
a folder, a disk image or a package. A **logo** (R1) goes on the game's page in
place of its name in plain text, and shows straight away.

Title `PPSA99318`, version 1.2.0. Tested on a PS5 with firmware 13.60.

## What it does

- **Lists your titles**: PS5 and PS4 games from the home screen's own art folder
  (`/user/appmeta`, including `external/`), and homebrew from `/data/homebrew` and
  the `homebrew` folders on USB and extended storage. Each one shows its art as
  it is now and what form that art takes (for example "Icon PNG 512×512, 2 files ·
  Background DDS BC7 3840×2160, 4 files").
- **Finds the game on SteamGridDB** by its name, and remembers the match. If the
  match is wrong, □ lists other matches or searches for any name you type.
- **Icons** come from SteamGridDB's square grids, and **Backgrounds** from its
  heroes (art made for backgrounds). A PS5 background is 3840×2160, so the
  3840×1240 heroes are listed first and tagged 4K: a 1920×620 one is enlarged
  about 3.5 times to fill it, and the frame page says so.
- **Adds a background** to homebrew that has none (△, "Add background"), when
  ShadowMountPlus installed it from a folder: Art Swap writes `pic0.dds`
  (3840×2160 BC7, as the console writes its own) into the game's `sce_sys`,
  and Apply changes installs the game again, since the home screen takes a
  background in only at an install. There is no original, so Restore deletes
  it (the files are listed in `/data/artswap/added.txt`).
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
- **Applies changes** (R2): the home screen keeps its own copy of each game's
  art, so it restarts to show new art, every game's at once. Only the home
  screen restarts: see [Apply changes](#apply-changes). The top of the screen
  says how many games are waiting, and a changed game's page says it is not
  on the home screen yet.
- **Logos** (R1): a game's page on the home screen shows its logo where it has
  one and its name in plain text where it has not, as with homebrew and
  installed backups. Art Swap puts a logo from SteamGridDB there, and it shows
  at once: see [Logos](#logos). Store games can also be offered their own
  logo from PlayStation Store, when you turn that on in Settings.
- **Renames homebrew** (L2) that ShadowMountPlus installed from a folder: the
  new name goes into the title's `param.json` (every language), and Art Swap
  has ShadowMountPlus install the game again, since the home screen takes a
  name only from an install: see [Renaming](#renaming). The original is kept
  as `param.json.old`, and Restore puts it back too.

## Installing

You need the console's homebrew environment running: etaHEN (or kstuff),
ShadowMountPlus, and an **ELF loader on port 9021**. Art Swap starts its helper
through that loader.

1. Download `PS5_ArtSwap-v1.2.0.zip` from [Releases](https://github.com/griches/PS5_ArtSwap/releases/latest).
2. Copy the `PPSA99318` folder inside it to `/data/homebrew/` (FTP is fine).
3. ShadowMountPlus registers it and Art Swap appears on the home screen.
4. Start it. The first screen says "Starting the helper" for a moment.
5. Add your SteamGridDB API key when it asks (see [below](#the-steamgriddb-api-key)).

## Using it

| Where | Button | What it does |
| --- | --- | --- |
| Titles | ← → | choose a title |
| | ✕ | change its icon |
| | △ | change its background, or add one to homebrew ShadowMountPlus installed from a folder |
| | R1 | change the logo on its page (a title the home screen lists) |
| | □ | restore its original art and name, and take its logo off (when Art Swap changed them) |
| | L2 | rename it (homebrew ShadowMountPlus installed from a folder; asks first) |
| | R2 | apply changes: restart the home screen so it shows them all (asks first) |
| | OPTIONS | settings: API key, look at the console again, PlayStation Store logos, helper log |
| | hold OPTIONS | close Art Swap |
| Art | L1 R1 | the Icons, Backgrounds and Logos tabs |
| | D-pad, left stick | choose a picture |
| | △ | the frame page: which part of the picture is kept (not for a logo, which is used whole) |
| | right stick | slide the part kept, when Fill cuts the picture |
| | ✕ | use it (asks first) |
| | □ | another game, or search by name with the keyboard |
| | ○ | back to the titles |
| Frame | L1 R1 | Fit, Left, Centre, Right (or Top, Middle, Bottom), Custom |
| | left stick, D-pad | move the part kept |
| | R2 L2 | zoom in, zoom out |
| | ✕ | use it (asks first) |
| | ○ | back to the pictures |

## Apply changes

The home screen draws each game's tile and background from its own copies of
the art (`/user/appmeta/<id>`, and `/user/app/<id>/sce_sys` for a game
ShadowMountPlus installed), and it keeps what it has drawn. Art Swap writes all
of those copies, as well as a homebrew folder's own `sce_sys`. To show them,
the home screen restarts: R2 opens a pop-up that says what will happen, and ✕
starts it.

1. Art Swap tells the helper, then closes.
2. Once Art Swap's process has ended, the helper ends the home screen's
   process (`SceShellUI`). The system starts it again within a few seconds.
3. The new home screen reads every game's art again, so every change shows,
   whatever installed the game.

Nothing else restarts: a game open in the background carries on, saved data is
not touched, and payloads such as ShadowMountPlus keep running (there is no
loader to run again). The helper keeps the titles changed since the last
restart in `/data/artswap/pending.txt`. A console restart is not needed.

A name is different: the home screen takes it only from an install, so a
restart of either kind does not show it. That is what Rename does.

## Renaming

L2 on homebrew that ShadowMountPlus installed from a folder asks for the new
name, writes it, and has ShadowMountPlus install the game again, which deleting
the title and letting ShadowMountPlus add it back would also do:

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
   it says how the rename's install went.

Nothing is deleted. If the game's folder had changed, the rename says so
rather than calling itself done (the folder itself is not copied: the
uninstall never touches it). The copy from the last install that went well is
kept for each title, and the copy from any that did not.

A rename needs **ShadowMountPlus 1.7 or later**, running, and a title it
installed **from a folder**. Art Swap never installs again a title installed
from a package (taking its tile away would delete the game) or from an image
(that would copy the image's own name and art back); the pop-up says why.

## Logos

A game's page on the home screen (the tile selected, with Play Game under it)
shows the game's logo, or else its name in plain text. The home screen draws
the picture its database names for the title (`logoImageInfo` in
`/system_data/priv/mms/app.db`). Store games get theirs from PlayStation
Network, which a jailbroken console does not reach, and homebrew has none.

R1, or the Logos tab of the picker, lists SteamGridDB's logos for the game
(transparent PNGs, mostly white lettering), each on a dark plate, with the
page as it will look beside the logo there now. ✕ puts it on:

1. The helper writes the PNG as `logo.png` beside the title's art in
   `/user/appmeta/<id>` (the home screen cannot read the homebrew folders), and
   keeps its own copy in `/data/artswap/logos/`.
2. It points the title's `logoImageInfo` at it, editing the database in place
   with SQLite, which waits for the home screen's own locks. It is the only
   thing Art Swap changes there. Before its first change the database is
   copied to `/data/artswap/app.db.first`, and before each run's first change
   to `app.db.backup`.
3. The page shows it straight away: Apply changes is not needed.

A logo stays through a console restart. An install writes the title's entry
afresh and takes the logo away, so after a rename's install (Apply changes)
the helper waits for the entry to come back and puts the logo back, and each
time Art Swap starts it puts back any of its logos that went missing. Restore
takes the logo off and puts back what the page had before (most often
nothing). A title the home screen does not list has no page, so no logo.

**PlayStation Store logos** (Settings, Games; off until you turn them on): for
a store game (one whose store concept is not just its own number, as
homebrew's is), the Logos tab starts with the game's own logo from
PlayStation Store's public catalog. The helper asks it as a web browser would,
with no account, key, cookie or anything that says Art Swap or PS5, but Sony
still sees the console's address and which game, so it is your choice. Each
game is asked about once; the answer is kept in `/data/artswap/store/`. Most
jailbroken consoles are kept from Sony's servers, to stop updates: on those
the store cannot be reached, the Logos tab says so once a run, and
SteamGridDB's logos show as before (it has the official logo of most store
games too). Settings says this under the switch.

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
| `pending.txt` | the titles changed since the home screen last restarted |
| `home.txt` | how the last Apply changes ended |
| `refresh.txt` | how the last rename's install ended |
| `saves/` | what each rename's install copied aside (see above) |
| `apikey.txt` | your SteamGridDB API key |
| `cache/` | downloaded pictures (trimmed to 192 MB when over 256 MB) |
| `logos/` | the logo Art Swap put on each title's page (`<id>.png`), and what it replaced (`<id>.orig`) |
| `added.txt` | the background files Art Swap added to titles that had none |
| `app.db.first`, `app.db.backup` | the home screen's database before Art Swap first changed it, and before its last run's first change |
| `store-logos` | if present, PlayStation Store logos are on |
| `store/` | what PlayStation Store's catalog said about each store game's logo |
| `no-appdb` | if present, the system's `app.db` is never read (no names from it, no logos) |

Names come from the title's `param.json` or `param.sfo`, then from a read-only
copy of the system's `app.db`, and otherwise the title id is shown. The same
copy says which titles the home screen lists and which are store games.

## Tested on the console

On a PS5 with firmware 13.60, ShadowMountPlus 1.7beta3: the title reaches the
helper on 127.0.0.1 and starts it through the ELF loader; the scan (20 titles
in 40-240 ms; `/user/appmeta/<id>/` holds `icon0.png`, `pic0.dds`, `pic1.dds`,
`param.json` for a homebrew title); HTTPS to SteamGridDB's API and CDN
(helper/tests/nettest.c); the key page from another machine on the network;
changing and restoring icons and backgrounds; the install again that a rename
uses (new art on the tile within seconds, the game's saves identical
afterwards, file for file); and, with ShadowMountPlus 1.7beta7, Apply changes:
new and restored art shown for a folder game, a disk image game (`.ffpfsc`)
and a package game, the home screen back within about three seconds of Art
Swap closing, and a game open in the background carrying on. Logos: one from
SteamGridDB on a homebrew game's page at once, with no restart; a store
logo on a disk image game kept through a console restart; and a logo put back
17 seconds into a rename's install, when the game's entry came back. The
PlayStation Store lookup was tested from a Mac only. A background added to a
homebrew emulator that had none, shown after Apply changes.

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
  foreground, Art Swap included, so a rename's install finishes from the home
  screen.
  Its game list says `installed` before the install has copied anything; the
  helper waits for the copies' change times instead.
- Folders a game writes (its saves, mode 0700) can be read only as the game's
  user, not even as root: the helper switches to the folder's owner to read
  them.
- The home screen registers a background only as `pic0.dds`: a title whose
  `sce_sys` has a `pic0.png` alone (PS5X360's, or one Art Swap wrote as PNG)
  has none on the home screen, and its `pic0Info` stays empty.
- An install writes the title's entry in `app.db` some seconds after
  ShadowMountPlus lists it as installed (17 s once), so the helper waits for
  the entry before it puts a logo back.
- OnionHEN keeps its toolbox inside the home screen's process. After Apply
  changes restarts it, OnionHEN takes it for coming out of rest mode, tries to
  load the toolbox again, can time out, and the console may then show "System
  Software Error" (CE-108262-9); OK dismisses it.

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
- The way to set a logo (the PNG beside the art, `logoImageInfo` in `app.db`)
  was found by ensamvarg360's [Spela](https://github.com/ensamvarg360/spela)
  (GPL-3.0).
- Art Swap's own icon and background were generated for this project
  (sources in `ps5/art/`).

Art Swap as a whole (the title and the helper) is GPL-3.0-or-later
([`LICENSE`](LICENSE)).
