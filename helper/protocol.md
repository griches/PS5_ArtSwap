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
| `HELLO` | text: `lan <address>` (when the console has one), `artswapd <version>`, `key user <last four characters>` or `key none`, `store on\|off` (PlayStation Store logos), `http <http2\|http1\|curl\|unavailable> <why>` |
| `APPS [rescan]` | text, one line a title: `A id name kind icon pic kept sgdb_id sgdb_name name_kept refresh concept logo listed added recent`. kind is `ps5`, `ps4` or `homebrew`; icon and pic describe the slot (`png 512x512 x2`: format, size, number of files) or are `-`; kept is 1 when an `.old` original of its art exists, name_kept when a `param.json.old` does; refresh is 1 when ShadowMountPlus lists the title as installed from a folder it can reach (REFRESH and RENAME), 0 when only a restart shows new art. From the home screen's database: concept is the title's PlayStation Store concept (0 for homebrew, whose concept is its own number); logo is `art` (Art Swap's), `other` (one of its own) or `-`; listed is 1 when the home screen has an entry for it, which a logo needs; added is 1 when its background is one Art Swap added (it had none); recent is its place in the home screen's own order (`lastAccessIndex`, higher for more recently used; 0 when unknown). The first APPS scans, after putting back any logo of Art Swap's the home screen lost; `rescan` does both again. |
| `ART id slot [w h]` | the slot's art now (`icon`, or `pic`: pic0 before pic1, the `/user/appmeta` copy first; `logo`: the page's logo), scaled down to fit w × h |
| `KEY [key]` | checks the key with SteamGridDB, then keeps it in `/data/artswap/apikey.txt`; no key removes it. Text: the `key` line, as HELLO's |
| `KEYSTATE` | text: the `key` line, as HELLO's |
| `PHONE [code]` | with a four-digit code, opens the key page on port 9118 (or 9128, 9138) for ten minutes (phone.c); text: `lan <address>`, `port <port>`. Without one, closes it. |
| `SEARCH term` | text: `G id name year verified`, SteamGridDB's matches, closest first |
| `MATCH id game name` | keeps the title's SteamGridDB game (`/data/artswap/matches.tsv`); game 0 forgets it |
| `LIST game kind [page]` | text: `P page total limit`, then `I id url thumb width height style author mime` a picture, and `H count` for icons hidden as too small. Kinds: `square`, `covers`, `wide`, `heroes`, `icons`, `logos`. Static pictures only, nothing marked NSFW. |
| `THUMB url [w h]` | the picture at url (SteamGridDB's hosts only, through the cache), scaled down to fit |
| `PREVIEW url slot mode panx pany w h [zoom]` | what `APPLY` would make, at w × h |
| `APPLY id slot url [mode panx pany [zoom]]` | writes the picture into every file of the slot; for `pic` on a title with none that ShadowMountPlus installed from a folder, adds `pic0.dds` (3840×2160 BC7) to the game's `sce_sys` instead, and the title waits for an install (PENDING `name`), which is when the home screen takes a background in. mode `cover` fills the frame and cuts the overflow: the part kept is the largest of the file's shape, made zoom (1-8, default 1) times smaller, and pan -1..1 across and down places it in the rest; `fit` shows all of it on a blurred fill. Text: `O path.old` for each original kept, `W path format size` for each file written |
| `RESTORE id [slot]` | moves each `.old` back over its file (slot `icon`, `pic`, `logo` or `all`; `all` puts the name and the logo back too), and deletes a background Art Swap added (`pic`, `all`), which waits for an install to leave the home screen. Text: `R path` a file |
| `PREF key [value]` | one of the title's own choices, such as `sort` (`/data/artswap/prefs.txt`): a key of letters and digits, a value of up to 63 characters. Text: `pref key value` |
| `LOGO id url` | the picture (a PNG from SteamGridDB, or the store's) as the logo on the title's home-screen page, where its name shows in plain text without one (logo.c): written as `logo.png` beside its art in `/user/appmeta`, and the title's `logoImageInfo` in the home screen's database pointed at it. The page shows it at once. Text: `O`, `W` lines as APPLY's |
| `STORELOGO id` | a PlayStation Store game's own logo from the store's public catalog (store.c), when the person has turned that on: text as LIST's, `P`, then one `I` line, or none when the store has none |
| `STORE [on\|off]` | PlayStation Store logos (`/data/artswap/store-logos`), off until turned on. Text: `store on\|off` |
| `RENAME id name` | for a title ShadowMountPlus installed from a folder (as REFRESH): name (1-127 bytes of UTF-8) as `titleName` in every language of each `param.json` beside its art, the original kept once as `param.json.old` (name.c). The home screen shows it after HOME has installed the title again. Text: `O`, `W` lines as APPLY's |
| `SMP id` | text: `smp <version>` (`-` when ShadowMountPlus's API does not answer), `game <source_type> <installed> <path>` when it lists the title, `can <1\|0> <why not>`, and the `refresh` line, as REFRESHSTATE's |
| `REFRESH id [wait]` | starts the home-screen refresh of a title ShadowMountPlus installed from a folder (smp.c), asking for up to wait seconds while a game keeps the console busy. Text: the `refresh` line |
| `REFRESHSTATE [seen]` | text: `refresh phase id unseen message`; phase `none`, `copying`, `waiting`, `removing`, `adding`, `checking`, `done` or `failed`. A finished refresh is unseen (1) until `REFRESHSTATE seen` |
| `PENDING` | text: `P id kind`, each title changed since the home screen last restarted (`/data/artswap/pending.txt`): kind `art` (APPLY, RESTORE) or `name` (RENAME, or RESTORE putting a name back), which HOME installs again first. A scan puts back a title Art Swap renamed whose name the home screen does not show yet |
| `HOME pid` | Apply changes: once the process pid (Art Swap, closing) has ended, the helper has ShadowMountPlus install each renamed title again (as REFRESH, saved data copied aside and checked), then ends `SceShellUI` and the system starts the home screen again, which reads every game's art afresh (home.c). Clears PENDING when it is back, but for renames whose install failed. Text: the `home` line, as HOMESTATE's |
| `HOMESTATE [seen]` | text: `home phase unseen message`; phase `none`, `waiting`, `installing`, `restarting`, `done` or `failed`. A finished restart is unseen (1) until `HOMESTATE seen` (`/data/artswap/home.txt`) |
| `LOG` | the last 8 KiB of `/data/artswap/helper.log` |
| `BYE` | the helper leaves (after a refresh or a home-screen restart that is running) |

## Rules the helper keeps

- It changes only the art files the scan found (`icon0`, `pic0`, `pic1`, as PNG,
  JPEG or DDS, in `/user/appmeta/<id>`, `/user/app/<id>/sce_sys` and the
  homebrew folder's `sce_sys`) of the title named, the `param.json` beside them for a name, and
  `logo.png` in its `/user/appmeta` folder for a logo, never a path from a request.
- It keeps each original once, as `<file>.old`, before the first change, and
  never writes an `.old` that exists. Files are written whole beside the target
  and renamed over it, with the original's mode and owner.
- It fetches pictures only from SteamGridDB's own hosts, and, with PlayStation
  Store logos on, Sony's image server. The store's catalog is asked as a web
  browser asks it: no key, no cookie, nothing that says Art Swap or PS5. Each
  game's answer is kept, so it is asked about once.
- In the home screen's database (`/system_data/priv/mms/app.db`) it changes
  only `logoImageInfo`, of a title the person gives a logo or restores, in
  place through SQLite. Before its first change it copies the database to
  `/data/artswap/app.db.first` (kept for good), and before each run's first
  change to `app.db.backup`. It keeps its own copy of each logo in
  `/data/artswap/logos`, and puts a logo back when an install has taken it.
- It keeps no key of its own: only the person's, in `/data/artswap/apikey.txt`.
- The key page listens on the network only between `PHONE code` and `PHONE`
  (or ten minutes, a saved key, or ten wrong codes), and saves only a key that
  comes with the code and that SteamGridDB accepts.
- It never asks ShadowMountPlus to uninstall a title installed from a package
  or an image, and removes nothing until the game's folder has been read whole
  and the title's saved data copied aside.
- It leaves 90 seconds after its last connection closes, or 10 minutes after
  starting if nobody connects, but never during a refresh or a home-screen restart.
