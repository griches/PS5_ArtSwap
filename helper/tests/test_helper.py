#!/usr/bin/env python3
# Art Swap helper - the whole protocol against a made-up console, with the real SteamGridDB.
#
#   helper/tools/build.sh host && python3 helper/tests/test_helper.py [WORK_DIR]
#
# Lays out a console's folders in WORK_DIR (default: a temporary folder):
# /user/appmeta with a PS5 title, a PS4 title (param.sfo), a title under
# external/ named only in app.db, a system app that must be left out, and
# /data/homebrew with a registered and an unregistered title. The PS5 title's
# art is a real one of ours (Eternal Darkness: icon0.png, BC7 pic0/pic1.dds).
# Then runs the host helper on it and checks every request, the files written
# (format, size, header, levels), the .old originals, and restoring them.
# Needs network access and a SteamGridDB key: SGDB_KEY in .env goes into the
# made-up console's /data/artswap/apikey.txt, as a person's own key would.
#
# Copyright (C) 2026 Gary Riches
# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import json
import os
import shutil
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
from client import Helper, HelperError  # noqa: E402
import fake_smp  # noqa: E402

ED = ROOT.parent / "PS5_EternalDarkness/ps5/sce_sys"
PORT = 19117
PHONE_PORT = 19118
SMP_PORT = 19101
failures = []


def env_key():
    env = ROOT / ".env"
    for line in env.read_text().splitlines() if env.exists() else []:
        if line.startswith("SGDB_KEY="):
            return line.split("=", 1)[1].strip()
    sys.exit("put SGDB_KEY=<your SteamGridDB key> in .env first")


def check(condition, what):
    print(("ok   " if condition else "FAIL ") + what)
    if not condition:
        failures.append(what)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def param_json(title_id, name):
    return json.dumps({"titleId": title_id, "localizedParameters": {"defaultLanguage": "en-US",
                                                                    "en-US": {"titleName": name}}})


def param_sfo(title):
    keys = b"TITLE\0"
    value = title.encode() + b"\0"
    key_start = 20 + 16
    data_start = key_start + len(keys) + (-len(keys)) % 4
    header = b"\0PSF" + struct.pack("<IIII", 0x101, key_start, data_start, 1)
    entry = struct.pack("<HHIII", 0, 0x0204, len(value), 128, 0)
    return header + entry + keys + b"\0" * ((-len(keys)) % 4) + value.ljust(128, b"\0")


def make_console(base: Path):
    meta = base / "user/appmeta"
    # a PS5 title of ours: art in appmeta and in its homebrew folder
    for folder in (meta / "PPSA99314", base / "data/homebrew/PPSA99314/sce_sys"):
        folder.mkdir(parents=True)
        for name in ("icon0.png", "pic0.dds", "pic1.dds"):
            shutil.copy(ED / name, folder / name)
        (folder / "param.json").write_text(param_json("PPSA99314", "Eternal Darkness™"))
    # a PS4 title: PNG art, the name in param.sfo
    ps4 = meta / "CUSA00001"
    ps4.mkdir(parents=True)
    Image.new("RGB", (512, 512), (40, 90, 160)).save(ps4 / "icon0.png")
    gradient = Image.linear_gradient("L").resize((1920, 1080)).convert("RGB")
    gradient.save(ps4 / "pic0.png")
    (ps4 / "param.sfo").write_bytes(param_sfo("Bloodborne"))
    # a title under external/, named only in app.db
    ext = meta / "external/PPSA05678"
    ext.mkdir(parents=True)
    Image.new("RGBA", (512, 512), (200, 30, 30, 255)).save(ext / "icon0.png")
    db_folder = base / "system_data/priv/mms"
    db_folder.mkdir(parents=True)
    db = sqlite3.connect(db_folder / "app.db")
    db.execute("CREATE TABLE tbl_appbrowse_0507646227 (titleId TEXT, titleName TEXT, metaDataPath TEXT)")
    db.execute("INSERT INTO tbl_appbrowse_0507646227 VALUES ('PPSA05678', 'Astro Bot', '/user/appmeta/external/PPSA05678')")
    db.commit()
    db.close()
    # a system app: left out
    sys_app = meta / "NPXS40000"
    sys_app.mkdir(parents=True)
    Image.new("RGB", (512, 512)).save(sys_app / "icon0.png")
    # an unregistered homebrew title, its folder not named by its id
    hb = base / "data/homebrew/Burnout3/sce_sys"
    hb.mkdir(parents=True)
    Image.new("RGB", (512, 512), (250, 120, 0)).save(hb / "icon0.png")
    (hb / "param.json").write_text(param_json("PPSA99316", "Burnout 3"))
    # a game ShadowMountPlus installed from a disk image: the home screen's copy in /user/app/<id>/sce_sys
    for folder in (meta / "PPSA26525", base / "user/app/PPSA26525/sce_sys"):
        folder.mkdir(parents=True)
        Image.new("RGB", (512, 512), (90, 40, 20)).save(folder / "icon0.png")
        (folder / "param.json").write_text(param_json("PPSA26525", "Heretic + Hexen"))
    (base / "data/artswap").mkdir(parents=True)


def wait_port(port, seconds=10):
    end = time.time() + seconds
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
            return True
        except OSError:
            time.sleep(0.1)
    return False


def dds_info(path):
    data = Path(path).read_bytes()
    return data[:148], len(data)


def main() -> int:
    work = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(tempfile.mkdtemp(prefix="artswap-test-"))
    if work.exists():
        shutil.rmtree(work)
    base = work / "console"
    make_console(base)
    shots = work / "shots"
    shots.mkdir()
    originals = {p: sha(p) for p in base.rglob("*") if p.is_file() and p.suffix in (".png", ".dds")}

    key = env_key()
    (base / "data/artswap/apikey.txt").write_text(key + "\n")
    env = dict(os.environ, ARTSWAP_ROOT=str(base), ARTSWAP_PORT=str(PORT), ARTSWAP_PHONE_PORT=str(PHONE_PORT),
               ARTSWAP_LAN="127.0.0.1", ARTSWAP_SMP_PORT=str(SMP_PORT))
    smp = fake_smp.serve(base, SMP_PORT)
    # a homebrew game's own save, and the system's saved data of the title
    save = base / "data/homebrew/PPSA99314/userdata/card.gci"
    save.parent.mkdir(parents=True)
    save.write_bytes(os.urandom(32832))
    sysave = base / "user/home/1000/savedata_prospero/PPSA99314/sdimg_SAVE0"
    sysave.parent.mkdir(parents=True)
    sysave.write_bytes(os.urandom(65536))
    helper_bin = ROOT / "build/helper-host/artswapd"
    log = open(work / "helper.out", "w")
    proc = subprocess.Popen([str(helper_bin)], env=env, stdout=log, stderr=subprocess.STDOUT)
    try:
        check(wait_port(PORT), "the helper listens")
        h = Helper(port=PORT)

        hello = dict(r[:2] for r in h.rows("HELLO"))
        check(hello.get("artswapd") == "0.3.2", f"HELLO names the version ({hello})")
        check(hello.get("key") == "user", "the person's key (apikey.txt) is in")
        check(hello.get("lan") == "127.0.0.1", "HELLO gives the console's address")

        apps = {r[1]: r for r in h.rows("APPS")}
        print("     " + "\n     ".join("\t".join(r) for r in apps.values()))
        check(set(apps) == {"PPSA99314", "CUSA00001", "PPSA05678", "PPSA99316", "PPSA26525"},
              "APPS lists the five titles, not the system app")
        ed = apps["PPSA99314"]
        check(ed[2] == "Eternal Darkness", "the name from param.json, trademark sign dropped")
        check(ed[3] == "homebrew", "a title with a homebrew folder is homebrew")
        check(ed[4] == "png 512x512 x2", f"two icons ({ed[4]})")
        check(ed[5] == "dds-bc7 3840x2160 x4", f"four backgrounds ({ed[5]})")
        check(apps["CUSA00001"][2] == "Bloodborne" and apps["CUSA00001"][3] == "ps4", "a PS4 title named from param.sfo")
        check(apps["PPSA05678"][2] == "Astro Bot", "a name from app.db")
        check(apps["PPSA99316"][2] == "Burnout 3", "an unregistered homebrew title by its param.json")
        check(ed[10] == "1", "APPS: a folder ShadowMountPlus installed can be refreshed")
        check(apps["CUSA00001"][10] == "0" and apps["PPSA26525"][10] == "0",
              "APPS: a package and an image cannot be installed again (no rename)")
        check(apps["PPSA26525"][4] == "png 512x512 x2", f"an image's icon: appmeta and /user/app/<id>/sce_sys ({apps['PPSA26525'][4]})")
        info = {r[0]: r for r in h.rows("SMP", "PPSA26525")}
        check(info["can"][1] == "0" and "disk image" in info["can"][2], f"SMP: an image cannot be renamed ({info['can']})")

        w, hh, rgba = h.ask("ART", "PPSA99314", "icon", 256, 256)
        check((w, hh) == (256, 256), "ART icon at 256")
        w, hh, rgba = h.ask("ART", "PPSA99314", "pic", 960, 540)
        check((w, hh) == (960, 540), "ART background (BC7) at 960x540")
        Image.frombytes("RGBA", (w, hh), rgba).save(shots / "ed-current-pic.png")

        games = h.rows("SEARCH", "Eternal Darkness")
        check(games and games[0][1] == "17", f"SEARCH finds Eternal Darkness first ({games[:2]})")
        h.ask("MATCH", "PPSA99314", "17", games[0][2])
        check(h.rows("APPS")[1 if False else 0] is not None and
              {r[1]: r for r in h.rows("APPS")}["PPSA99314"][7] == "17", "MATCH is kept")
        check((base / "data/artswap/matches.tsv").read_text().startswith("PPSA99314\t17\t"), "matches.tsv written")

        lists = {}
        for kind in ("square", "covers", "heroes", "icons", "wide", "logos"):
            rows = h.rows("LIST", 17, kind, 0)
            items = [r for r in rows if r[0] == "I"]
            lists[kind] = items
            check(rows[0][0] == "P", f"LIST {kind}: {len(items)} pictures, page line {rows[0]}")
        check(len(lists["square"]) >= 1 and len(lists["heroes"]) >= 1, "square art and heroes exist for ED")
        check(all(int(r[4]) >= 192 for r in lists["icons"]), "small icons are hidden")

        square, hero = lists["square"][0], lists["heroes"][0]
        w, hh, rgba = h.ask("THUMB", square[3], 256, 256)
        check(max(w, hh) <= 256, f"THUMB fits 256 ({w}x{hh})")
        w, hh, rgba = h.ask("PREVIEW", hero[2], "pic", "cover", "0", "0", 960, 540)
        check((w, hh) == (960, 540), "PREVIEW cover at 960x540")
        Image.frombytes("RGBA", (w, hh), rgba).save(shots / "preview-cover.png")
        w, hh, rgba = h.ask("PREVIEW", hero[2], "pic", "fit", "0", "0", 960, 540)
        Image.frombytes("RGBA", (w, hh), rgba).save(shots / "preview-fit.png")
        # zoom: twice as close at the top left is the top-left quarter of the whole frame at twice the size
        w, hh, rgba = h.ask("PREVIEW", hero[2], "pic", "cover", "-1", "-1", 960, 540, "2")
        close = Image.frombytes("RGBA", (w, hh), rgba).convert("RGB")
        close.save(shots / "preview-zoom.png")
        w2, hh2, rgba = h.ask("PREVIEW", hero[2], "pic", "cover", "-1", "-1", 1920, 1080)
        quarter = Image.frombytes("RGBA", (w2, hh2), rgba).convert("RGB").crop((0, 0, 960, 540))
        diff = sum(abs(a - b) for pa, pb in zip(close.get_flattened_data(), quarter.get_flattened_data()) for a, b in zip(pa, pb))
        check(diff / (960 * 540 * 3) < 3, f"PREVIEW zoom 2 is the quarter it should be (mean difference {diff / (960 * 540 * 3):.2f})")
        cover = lists["covers"][0]
        w, hh, rgba = h.ask("PREVIEW", cover[2], "icon", "fit", "0", "0", 512, 512)
        Image.frombytes("RGBA", (w, hh), rgba).save(shots / "preview-icon-fit.png")

        try:
            h.ask("THUMB", "https://example.com/x.png", 64, 64)
            check(False, "a picture from elsewhere is refused")
        except HelperError:
            check(True, "a picture from elsewhere is refused")

        # the icon
        icon_meta = base / "user/appmeta/PPSA99314/icon0.png"
        icon_hb = base / "data/homebrew/PPSA99314/sce_sys/icon0.png"
        t = time.time()
        report = h.rows("APPLY", "PPSA99314", "icon", square[2], "cover", "0", "0")
        print(f"     icon applied in {time.time() - t:.1f} s: {report}")
        check(sum(r[0] == "W" for r in report) == 2 and sum(r[0] == "O" for r in report) == 2, "APPLY icon writes two and keeps two originals")
        for p in (icon_meta, icon_hb):
            im = Image.open(p)
            check(im.size == (512, 512) and im.format == "PNG", f"{p.relative_to(base)} is a 512x512 PNG ({im.mode})")
            check(sha(str(p) + ".old") == originals[p], f"{p.name}.old is the original")
        shutil.copy(icon_meta, shots / "ed-new-icon0.png")

        # the background: BC7, the header kept, four files
        t = time.time()
        report = h.rows("APPLY", "PPSA99314", "pic", hero[2], "cover", "0.3", "0")
        print(f"     background applied in {time.time() - t:.1f} s")
        check(sum(r[0] == "W" for r in report) == 4, "APPLY pic writes pic0 and pic1, both copies")
        for p in sorted(base.rglob("pic*.dds")):
            header, size = dds_info(p)
            orig_header, orig_size = dds_info(str(p) + ".old")
            check(header == orig_header and size == orig_size == 8294548, f"{p.relative_to(base)}: same header, same size")
        im = Image.open(base / "user/appmeta/PPSA99314/pic0.dds")
        im.load()
        check(im.size == (3840, 2160), "Pillow reads the new BC7 background")
        im.convert("RGB").resize((960, 540)).save(shots / "ed-new-pic0.png")

        # again: the originals stay the first ones
        h.rows("APPLY", "PPSA99314", "icon", lists["square"][-1][2], "cover", "0", "0")
        check(sha(str(icon_meta) + ".old") == originals[icon_meta], "a second change keeps the first original")

        # the home screen's copy made again from the folder without its .old (an install whose copy-back
        # did not happen): the next change keeps the folder's original for it, not the art shown now
        Path(str(icon_meta) + ".old").unlink()
        shutil.copy(icon_hb, icon_meta)
        h.rows("APPLY", "PPSA99314", "icon", square[2], "cover", "0", "0")
        check(sha(str(icon_meta) + ".old") == originals[icon_meta], "a copy that lost its .old keeps the folder's original")

        # a PS4 title's PNG background (RGB), fit mode
        report = h.rows("APPLY", "CUSA00001", "pic", hero[2], "fit", "0", "0")
        im = Image.open(base / "user/appmeta/CUSA00001/pic0.png")
        check(im.size == (1920, 1080) and im.mode == "RGB", f"PS4 pic0.png stays 1920x1080 RGB ({im.mode})")
        im.save(shots / "ps4-new-pic0-fit.png")

        apps = {r[1]: r for r in h.rows("APPS")}
        check(apps["PPSA99314"][6] == "1", "APPS says the originals are kept")

        # Apply changes: the titles changed are waiting; HOME restarts the home screen once Art Swap has closed
        pending = {r[1] for r in h.rows("PENDING") if r[0] == "P"}
        check({"PPSA99314", "CUSA00001"} <= pending, f"PENDING lists the titles changed ({pending})")
        marker = base / "data/artswap/home-restarted"
        title = subprocess.Popen(["sleep", "2"])  # Art Swap, closing
        home = next(r for r in h.rows("HOME", str(title.pid)) if r[0] == "home")
        check(home[1] == "waiting", f"HOME waits for Art Swap to close ({home})")
        time.sleep(1.0)
        check(not marker.exists(), "nothing restarts while Art Swap is open")
        title.wait()
        end = time.time() + 10
        while time.time() < end and next(r for r in h.rows("HOMESTATE") if r[0] == "home")[1] != "done":
            time.sleep(0.2)
        home = next(r for r in h.rows("HOMESTATE") if r[0] == "home")
        check(home[1] == "done" and home[2] == "1" and marker.exists(), f"the home screen restarts once it has closed ({home})")
        check(not [r for r in h.rows("PENDING") if r[0] == "P"], "nothing is waiting after the restart")
        h.rows("HOMESTATE", "seen")
        check(next(r for r in h.rows("HOMESTATE") if r[0] == "home")[2] == "0", "HOMESTATE seen: shown")
        check("done\t0\t" in (base / "data/artswap/home.txt").read_text(), "home.txt keeps it as seen")

        # the home-screen refresh, through ShadowMountPlus (fake_smp.py)
        def refresh_state():
            return next(r for r in h.rows("REFRESHSTATE") if r[0] == "refresh")

        def refresh_until_end(seconds=40):
            end = time.time() + seconds
            while time.time() < end:
                state = refresh_state()
                if state[1] in ("done", "failed"):
                    return state
                time.sleep(0.25)
            return refresh_state()

        info = {r[0]: r for r in h.rows("SMP", "PPSA99314")}
        check(info["smp"][1] == "1.7fake" and info["can"][1] == "1", f"SMP: a folder title can be refreshed ({info})")
        info = {r[0]: r for r in h.rows("SMP", "CUSA00001")}
        check(info["can"][1] == "0" and "package" in info["can"][2], f"SMP: never a package ({info['can']})")
        try:
            h.rows("REFRESH", "CUSA00001", "0")
            check(False, "REFRESH refuses a package")
        except HelperError as e:
            check("package" in str(e), f"REFRESH refuses a package ({e})")

        icon_old = sha(str(icon_meta) + ".old")
        save_sha, sysave_sha = sha(save), sha(sysave)
        original_save = save.read_bytes()
        smp.console.busy = True
        h.rows("REFRESH", "PPSA99314", "0")
        state = refresh_until_end()
        check(state[1] == "failed" and "Device busy" in state[4], f"a busy console: the refresh stops ({state})")
        check((base / "user/appmeta/PPSA99314/icon0.png").exists(), "a busy console: nothing is removed")
        check(not list((base / "data/artswap/saves").glob("PPSA99314-*")), "a busy console: no copy is left over")

        # the title closes: the refresh waits for the console to be free
        h.rows("REFRESH", "PPSA99314", "20")
        time.sleep(1.5)
        check(refresh_state()[1] == "waiting", "the refresh waits while the console is busy")
        smp.console.busy = False
        state = refresh_until_end()
        check(state[1] == "done", f"the refresh is done ({state})")
        check(state[3] == "1", "the end is kept for the title's next start")
        check((base / "user/appmeta/PPSA99314/param.json").exists(), "the title is back on the home screen")
        check(sha(str(icon_meta) + ".old") == icon_old, "Art Swap's original in the home screen's copy is back")
        check(sha(save) == save_sha, "the game's own save is untouched")
        check(sha(sysave) == sysave_sha, "the system's saved data is untouched")
        kept = list((base / "data/artswap/saves").glob("PPSA99314-*"))
        check(len(kept) == 1 and (kept[0] / "home/1000/savedata_prospero/sdimg_SAVE0").exists(),
              "the system's saved data was copied aside first")
        check(not (kept[0] / "folder").exists(), "the game's folder is not copied")
        check(smp.console.calls.count("/api/v1/scan") >= 1, "ShadowMountPlus was asked to scan")
        h.rows("REFRESHSTATE", "seen")
        check(refresh_state()[3] == "0", "REFRESHSTATE seen: shown")
        check("\t0\t" in (base / "data/artswap/refresh.txt").read_text(), "refresh.txt keeps it as seen")

        # an uninstall that eats a save of the folder: the refresh says so (there is no copy to put back)
        smp.console.eat = "userdata/card.gci"
        time.sleep(1.1)
        h.rows("REFRESH", "PPSA99314", "0")
        state = refresh_until_end()
        smp.console.eat = ""
        check(state[1] == "failed" and "folder changed" in state[4], f"a lost save of the folder is reported ({state[4]})")
        check(not save.exists(), "the folder's save is not made up")
        save.write_bytes(original_save)

        # one that eats the system's saved data: it is put back, and the refresh says so
        smp.console.eat = "../../../user/home/1000/savedata_prospero/PPSA99314/sdimg_SAVE0"
        time.sleep(1.1)
        h.rows("REFRESH", "PPSA99314", "0")
        state = refresh_until_end()
        smp.console.eat = ""
        check(state[1] == "failed" and "saved data changed" in state[4], f"lost saved data is reported ({state[4]})")
        check(sysave.exists() and sha(sysave) == sysave_sha, "the lost saved data is copied back")
        check(len(list((base / "data/artswap/saves").glob("PPSA99314-*"))) == 3,
              "the copies of failed refreshes are kept")

        # a new name, in every param.json, each original kept once
        folder_param = base / "data/homebrew/PPSA99314/sce_sys/param.json"
        meta_param = base / "user/appmeta/PPSA99314/param.json"
        original_param = folder_param.read_text()

        def names(path):
            return {v["titleName"] for v in json.loads(path.read_text())["localizedParameters"].values()
                    if isinstance(v, dict)}

        report = h.rows("RENAME", "PPSA99314", "Eternal Darkness: Sanity's Requiem")
        check(sum(r[0] == "W" for r in report) == 2 and sum(r[0] == "O" for r in report) == 2,
              f"RENAME writes both param.json files and keeps both originals ({report})")
        check(names(folder_param) == names(meta_param) == {"Eternal Darkness: Sanity's Requiem"}, "the new name is in both")
        h.rows("RENAME", "PPSA99314", "Eternal Darkness é")
        check(Path(str(folder_param) + ".old").read_text() == original_param, "a second rename keeps the first original")
        apps = {r[1]: r for r in h.rows("APPS")}
        check(apps["PPSA99314"][2] == "Eternal Darkness é" and apps["PPSA99314"][9] == "1",
              f"APPS: the new name, its original kept ({apps['PPSA99314']})")
        for title, name, why in (("CUSA00001", "Bloodborne 2", "package"), ("PPSA99314", "a\x01b", "character")):
            try:
                h.rows("RENAME", title, name)
                check(False, f"RENAME refuses {why}")
            except HelperError as e:
                check(why in str(e), f"RENAME refuses: {e}")
        # Apply changes installs the renamed title again first, then restarts the home screen
        pend = {r[1]: r[2] for r in h.rows("PENDING") if r[0] == "P"}
        check(pend.get("PPSA99314") == "name", f"PENDING: the rename waits for an install ({pend})")
        marker.unlink(missing_ok=True)
        title = subprocess.Popen(["sleep", "1"])  # Art Swap, closing
        h.rows("HOME", str(title.pid))
        title.wait()
        end = time.time() + 90
        while time.time() < end and next(r for r in h.rows("HOMESTATE") if r[0] == "home")[1] not in ("done", "failed"):
            time.sleep(0.3)
        home = next(r for r in h.rows("HOMESTATE") if r[0] == "home")
        check(home[1] == "done" and marker.exists(), f"Apply changes after a rename is done ({home})")
        check(names(meta_param) == {"Eternal Darkness é"}, "the install took the new name")
        check(Path(str(meta_param) + ".old").exists(), "the home screen's original name is kept across the install")
        check(not [r for r in h.rows("PENDING") if r[0] == "P"], "nothing is waiting after Apply changes")
        h.rows("HOMESTATE", "seen")

        # restore; a copy without its .old takes the folder's original
        Path(str(base / "user/appmeta/PPSA99314/pic0.dds") + ".old").unlink()
        # the install copied the folder's sce_sys, its .old files too, into the home screen's copy; a scan
        # finds that copy, and Restore puts its originals back as well, for a restart to show them
        h.rows("APPS", "rescan")
        installed_olds = len(list((base / "user/app/PPSA99314/sce_sys").glob("*.old")))
        check(installed_olds == 4, f"the installed copy has the folder's originals ({installed_olds})")
        report = h.rows("RESTORE", "PPSA99314", "all")
        check(sum(r[0] == "R" for r in report) == 8 + installed_olds,
              f"RESTORE puts six art files, two names and the installed copy back ({len(report)})")
        check(folder_param.read_text() == original_param and names(meta_param) == {"Eternal Darkness™"},
              "the original name is back")
        check({r[1]: r for r in h.rows("APPS")}["PPSA99314"][2] == "Eternal Darkness",
              "APPS: the original name (its trademark sign dropped)")
        for p in list(base.rglob("PPSA99314/**/*.png")) + list(base.rglob("PPSA99314/**/*.dds")):
            if p in originals:
                check(sha(p) == originals[p], f"{p.relative_to(base)} is the original again")
        check(not list(base.rglob("PPSA99314/**/*.old")), "no .old files left")
        try:
            h.rows("RESTORE", "PPSA99314", "all")
            check(False, "a second restore says there is nothing")
        except HelperError as e:
            check("no original" in str(e), f"a second restore says there is nothing ({e})")

        # the key
        try:
            h.ask("KEY", "0123456789abcdef0123456789abcdef")
            check(False, "a wrong key is refused")
        except HelperError as e:
            check("401" in str(e) or "refused" in str(e), f"a wrong key is refused ({e})")
        check("key\tnone" in h.ask("KEY", ""), "KEY with nothing removes the key")
        check(not (base / "data/artswap/apikey.txt").exists(), "apikey.txt is gone")
        try:
            h.rows("SEARCH", "Portal")
            check(False, "with no key, SteamGridDB is not asked")
        except HelperError as e:
            check("No SteamGridDB key" in str(e), f"with no key, the answer says how to add one ({e})")

        # the key page, from a phone
        page = f"http://127.0.0.1:{PHONE_PORT}"
        try:
            urllib.request.urlopen(page + "/1234", timeout=2)
            check(False, "nothing listens before PHONE")
        except (urllib.error.URLError, ConnectionError, OSError):
            check(True, "nothing listens before PHONE")
        rows = dict(r[:2] for r in h.rows("PHONE", "4831"))
        check(rows.get("lan") == "127.0.0.1" and rows.get("port") == str(PHONE_PORT), f"PHONE opens the page ({rows})")
        time.sleep(0.5)
        html = urllib.request.urlopen(page + "/4831", timeout=5).read().decode()
        check("Add your SteamGridDB API key" in html and "name=code value='4831'" in html, "the QR code's page carries the code")
        html = urllib.request.urlopen(page + "/", timeout=5).read().decode()
        check("The code on your TV" in html, "the plain address asks for the code")
        (work / "shots/phone-page.html").write_text(urllib.request.urlopen(page + "/4831", timeout=5).read().decode())

        def post(code, value):
            data = urllib.parse.urlencode({"code": code, "key": value}).encode()
            try:
                response = urllib.request.urlopen(page + "/", data=data, timeout=30)
                return response.status, response.read().decode()
            except urllib.error.HTTPError as e:
                return e.code, e.read().decode()

        status, html = post("1111", key)
        check(status == 403 and "not the code on your TV" in html, f"a wrong code is refused ({status})")
        check("key\tnone" in h.ask("KEYSTATE"), "a wrong code saves nothing")
        status, html = post("4831", "0123456789abcdef0123456789abcdef")
        check(status == 200 and "did not accept that key" in html, "a key SteamGridDB refuses is not kept")
        check("key\tnone" in h.ask("KEYSTATE"), "... and nothing is saved")
        status, html = post("4831", key)
        check(status == 200 and "Your key is saved" in html, f"the right code and key are saved ({status})")
        (work / "shots/phone-done.html").write_text(html)
        check("key\tuser" in h.ask("KEYSTATE"), "KEYSTATE says the key is in")
        check((base / "data/artswap/apikey.txt").read_text().strip() == key, "apikey.txt holds the key")
        time.sleep(1.0)
        try:
            urllib.request.urlopen(page + "/4831", timeout=2)
            check(False, "the page closes once the key is in")
        except (urllib.error.URLError, ConnectionError, OSError):
            check(True, "the page closes once the key is in")
        h.rows("PHONE", "5555")
        time.sleep(0.5)
        h.ask("PHONE")
        time.sleep(1.0)
        try:
            urllib.request.urlopen(page + "/5555", timeout=2)
            check(False, "PHONE alone closes the page")
        except (urllib.error.URLError, ConnectionError, OSError):
            check(True, "PHONE alone closes the page")

        log_tail = (base / "data/artswap/helper.log").read_bytes()[-8192:].decode(errors="replace")
        check(log_tail and h.ask("LOG").strip() == log_tail.strip(), "LOG gives the last 8 KiB of the helper's log")
        h.ask("BYE")
        proc.wait(timeout=10)
        check(proc.returncode == 0, "BYE ends the helper")
    finally:
        if proc.poll() is None:
            proc.kill()
        log.close()
    print(f"\n{len(failures)} failed" if failures else "\nall passed")
    print(f"work folder: {work}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
