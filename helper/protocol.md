# The helper's protocol

`artswapd` listens on TCP 127.0.0.1:9117, or the first free of 9127 and 9137 (a
helper that crashed on the console keeps its port), on all addresses if
`/data/artswap/listen-lan` exists, for tests from another machine). A
connection carries any number of requests, one at a time.

**A request** is one line: fields separated by tabs, ending in `\n`.

**An answer** is a line, then perhaps a body:

- `OK <length> 0 0\n`, then `<length>` bytes of text: lines of tab-separated fields;
- `OK <length> <width> <height>\n`, then `<width> × <height> × 4` bytes of RGBA (top row first, straight alpha);
- `ERR <message>\n`, a sentence to show a person.

`helper/tests/client.py` speaks it (`client.py --port 9117 HELLO`).

## Requests

| Request | Answer |
| --- | --- |
| `HELLO` | text: `lan <address>` (when the console has one), `artswapd <version>`, `key user <last four characters>` or `key none`, `http <http2\|http1\|curl\|unavailable> <why>` |
| `APPS [rescan]` | text, one line a title: `A id name kind icon pic kept sgdb_id sgdb_name`. kind is `ps5`, `ps4` or `homebrew`; icon and pic describe the slot (`png 512x512 x2`: format, size, number of files) or are `-`; kept is 1 when an `.old` original exists. The first APPS scans; `rescan` scans again. |
| `ART id slot [w h]` | the slot's art now (`icon`, or `pic`: pic0 before pic1, the `/user/appmeta` copy first), scaled down to fit w × h |
| `KEY [key]` | checks the key with SteamGridDB, then keeps it in `/data/artswap/apikey.txt`; no key removes it. Text: the `key` line, as HELLO's |
| `KEYSTATE` | text: the `key` line, as HELLO's |
| `PHONE [code]` | with a four-digit code, opens the key page on port 9118 (or 9128, 9138) for ten minutes (phone.c); text: `lan <address>`, `port <port>`. Without one, closes it. |
| `SEARCH term` | text: `G id name year verified`, SteamGridDB's matches, closest first |
| `MATCH id game name` | keeps the title's SteamGridDB game (`/data/artswap/matches.tsv`); game 0 forgets it |
| `LIST game kind [page]` | text: `P page total limit`, then `I id url thumb width height style author mime` a picture, and `H count` for icons hidden as too small. Kinds: `square`, `covers`, `wide`, `heroes`, `icons`, `logos`. Static pictures only, nothing marked NSFW. |
| `THUMB url [w h]` | the picture at url (SteamGridDB's hosts only, through the cache), scaled down to fit |
| `PREVIEW url slot mode panx pany w h` | what `APPLY` would make, at w × h |
| `APPLY id slot url [mode panx pany]` | writes the picture into every file of the slot. mode `cover` fills the frame and cuts the overflow (pan -1..1 picks the part kept); `fit` shows all of it on a blurred fill. Text: `O path.old` for each original kept, `W path format size` for each file written |
| `RESTORE id [slot]` | moves each `.old` back over its file (slot `icon`, `pic` or `all`). Text: `R path` a file |
| `SMP id` | text: `smp <version>` (`-` when ShadowMountPlus's API does not answer), `game <source_type> <installed> <path>` when it lists the title, `can <1\|0> <why not>`, and the `refresh` line, as REFRESHSTATE's |
| `REFRESH id [wait]` | starts the home-screen refresh of a title ShadowMountPlus installed from a folder (smp.c), asking for up to wait seconds while a game keeps the console busy. Text: the `refresh` line |
| `REFRESHSTATE [seen]` | text: `refresh phase id unseen message`; phase `none`, `copying`, `waiting`, `removing`, `adding`, `checking`, `done` or `failed`. A finished refresh is unseen (1) until `REFRESHSTATE seen` |
| `LOG` | the last 8 KiB of `/data/artswap/helper.log` |
| `BYE` | the helper leaves (after a refresh that is running) |

## Rules the helper keeps

- It changes only the art files the scan found (`icon0`, `pic0`, `pic1`, as PNG,
  JPEG or DDS) of the title named, never a path from a request.
- It keeps each original once, as `<file>.old`, before the first change, and
  never writes an `.old` that exists. Files are written whole beside the target
  and renamed over it, with the original's mode and owner.
- It fetches pictures only from SteamGridDB's own hosts.
- It keeps no key of its own: only the person's, in `/data/artswap/apikey.txt`.
- The key page listens on the network only between `PHONE code` and `PHONE`
  (or ten minutes, a saved key, or ten wrong codes), and saves only a key that
  comes with the code and that SteamGridDB accepts.
- It never asks ShadowMountPlus to uninstall a title installed from a package
  or an image, and removes nothing until the game's folder has been read whole
  and the title's saved data copied aside.
- It leaves 90 seconds after its last connection closes, or 10 minutes after
  starting if nobody connects, but never during a refresh.
