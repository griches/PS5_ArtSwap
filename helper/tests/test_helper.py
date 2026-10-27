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
# (format, size, header, levels), the .old originals, and restoring them; the
# person's own pictures; and, on a console of its own, the home screen's
# layout (a made-up database with two users, then a copy of Gary's real one
# when this machine has it).
# Needs network access and a SteamGridDB key: SGDB_KEY in .env goes into the
# made-up console's /data/artswap/apikey.txt, as a person's own key would.
#
# Copyright (C) 2026 Gary Riches
# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import json
import os
import re
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


# the console's user, as in the home screen's per-user tables
USER = "0349465199"
TITLE_KEY = re.compile(r"TITLE(_\d\d)?")


def console_json(fields):
    """JSON as the console writes it into app.db: keys in order, no spaces, UTF-8 as it is, / escaped"""
    return json.dumps(fields, separators=(",", ":"), ensure_ascii=False, sort_keys=True).replace("/", "\\/")


def app_info(title, name, concept, languages):
    """A title's AppInfoJson, after the console's (64-bit numbers, a name for each language)"""
    fields = {"#_access_index": 1499, "#_install_time": "2026-10-22 22:46:15.772", "#_size": 371785728,
              "CONCEPT_ID": concept, "CONTENT_ID": f"UP9000-{title}_00-ARTSWAP000000000", "DOWNLOAD_DATA_SIZE": 256,
              "TITLE": name, "TITLE_ID": title, "_install_version": 1396115914549624833,
              "_metadata_path": f"/user/app/{title}/sce_sys", "_path_icon0_info": -8070450532247928832,
              "_path_pic0_info": -6917529027641081856, "_primary_title_sort": 197637, "sync_index": 1320}
    fields.update({f"TITLE_{language}": text for language, text in languages.items()})
    return console_json(fields)


def concept_info(title, name, concept, languages):
    """A concept's ConceptInfoJson: a list of fields, each string's length in bytes beside it"""
    def field(key, data, kind):
        return {"data": data, "key": key, "size": len(data.encode()) if kind == 2 else 8, "type": kind}
    names = [field("concept_name", name, 2)] + [field(f"concept_name_{language}", text, 2)
                                                for language, text in languages.items()]
    fields = [field("concept_id", concept, 0)] + names + [
        field("metadata_path", f"/user/app/{title}/sce_sys", 2), field("path_pic0_info", -6917529027641081856, 0),
        field("primary_title_id", title, 2), field("total_size", 371785728, 0)]
    return console_json({"field_list": fields, "mimeType": 0, "mimeTypeFormatVersion": 0, "parserId": 0,
                         "promoterFormatVersion": 0})


def home_names(base, title):
    """Every name the home screen's database keeps of a title, whether each concept name's size is right, and the
    two JSON texts"""
    db = sqlite3.connect(base / "system_data/priv/mms/app.db")
    name, info, local = db.execute("SELECT titleName, AppInfoJson, localConceptId FROM tbl_contentinfo "
                                   "WHERE titleId = ?", (title,)).fetchone()
    concept_name, concept = db.execute("SELECT conceptName, ConceptInfoJson FROM tbl_conceptmetadata "
                                       "WHERE localConceptId = ?", (local,)).fetchone()
    icon = db.execute(f"SELECT titleName FROM tbl_iconinfo_{USER} WHERE titleId = ?", (title,)).fetchone()[0]
    tile = db.execute(f"SELECT conceptName, primaryTitleName FROM tbl_concepticoninfo_{USER} "
                      "WHERE primaryTitleId = ?", (title,)).fetchone()
    db.close()
    fields = [f for f in json.loads(concept)["field_list"] if f["key"].startswith("concept_name")]
    names = {name, concept_name, icon, *tile} | {v for k, v in json.loads(info).items() if TITLE_KEY.fullmatch(k)} | \
        {f["data"] for f in fields}
    return names, all(f["size"] == len(f["data"].encode()) for f in fields), info, concept


def without_names(info, concept):
    """The two JSON texts' contents, names left out: what a rename must not change"""
    a = {k: v for k, v in json.loads(info).items() if not TITLE_KEY.fullmatch(k)}
    b = [f for f in json.loads(concept)["field_list"] if not f["key"].startswith("concept_name")]
    return a, b


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
    # the home screen's entries, as the console's: a homebrew title's concept is its own number, a store game's
    # is the store's; Burnout 3 is not on the home screen
    # with the console's columns for names, the background and music (pic0Info, snd0Info), and the per-user tables
    db.execute("CREATE TABLE tbl_contentinfo (titleId TEXT, titleName TEXT COLLATE NOCASE, conceptId INT, "
               "localConceptId TEXT, lastAccessIndex INT, AppInfoJson TEXT, pic0Info TEXT, snd0Info TEXT)")
    db.execute("CREATE TABLE tbl_conceptmetadata (localConceptId TEXT NOT NULL, conceptId INT, conceptName TEXT, "
               "logoImageInfo TEXT, ConceptInfoJson TEXT, PRIMARY KEY(localConceptId))")
    db.execute(f"CREATE TABLE tbl_iconinfo_{USER} (titleId TEXT, titleName TEXT COLLATE NOCASE, localConceptId TEXT, "
               "conceptId INT)")
    db.execute(f"CREATE TABLE tbl_concepticoninfo_{USER} (localConceptId TEXT, conceptId INT, conceptName TEXT, "
               "primaryTitleId TEXT, primaryTitleName TEXT)")
    for title, name, concept, pic0 in (("PPSA99314", "Eternal Darkness™", 99314, "/user/appmeta/PPSA99314/pic0.dds"),
                                       ("CUSA00001", "Bloodborne", 10000176, ""), ("PPSA05678", "Astro Bot", 10002691, ""),
                                       ("PPSA26525", "Heretic + Hexen", 10012903, ""), ("PPSA99131", "PSP5", 99131, "")):
        local = f"cid:scp:{concept:016x}"
        # Eternal Darkness has a Chinese name too, as Astro's Playroom has on the console
        languages = {"01": name, "11": "永恒的黑暗"} if title == "PPSA99314" else {"01": name}
        db.execute("INSERT INTO tbl_contentinfo VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                   (title, name, concept, local, concept % 1000, app_info(title, name, concept, languages),
                    pic0 + "?ts=1792709174" if pic0 else "", ""))
        db.execute("INSERT INTO tbl_conceptmetadata VALUES (?, ?, ?, NULL, ?)",
                   (local, concept, name, concept_info(title, name, concept, languages)))
        db.execute(f"INSERT INTO tbl_iconinfo_{USER} VALUES (?, ?, ?, ?)", (title, name, local, concept))
        db.execute(f"INSERT INTO tbl_concepticoninfo_{USER} VALUES (?, ?, ?, ?, ?)", (local, concept, name, title, name))
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
    # a folder game with an icon and no background (PSP5)
    for folder in (meta / "PPSA99131", base / "data/homebrew/PPSA99131/sce_sys", base / "user/app/PPSA99131/sce_sys"):
        folder.mkdir(parents=True)
        Image.new("RGB", (512, 512), (20, 60, 200)).save(folder / "icon0.png")
        (folder / "param.json").write_text(param_json("PPSA99131", "PSP5"))
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


def make_song(path, seconds=40, rate=44100):
    """A stereo WAV that sounds like something: a chord, a beat, a slow swell"""
    import math
    import wave
    frames = bytearray()
    for i in range(int(seconds * rate)):
        t = i / rate
        swell = 0.5 + 0.5 * math.sin(2 * math.pi * t / 8)
        beat = math.exp(-((t * 2) % 1) * 12)
        left = 0.25 * swell * (math.sin(2 * math.pi * 220 * t) + 0.6 * math.sin(2 * math.pi * 277.2 * t)) + 0.2 * beat * math.sin(2 * math.pi * 60 * t)
        right = 0.25 * swell * (math.sin(2 * math.pi * 330 * t) + 0.6 * math.sin(2 * math.pi * 440 * t)) + 0.2 * beat * math.sin(2 * math.pi * 60 * t)
        frames += struct.pack("<hh", int(max(-1, min(1, left)) * 32767), int(max(-1, min(1, right)) * 32767))
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames))


def at9_loudness(path):
    """(seconds, integrated LUFS) of an AT9 file, as FFmpeg's own ATRAC9 decoder hears it"""
    import re
    out = subprocess.run(["ffmpeg", "-hide_banner", "-nostats", "-i", str(path), "-af", "ebur128", "-f", "null", "-"],
                         capture_output=True, text=True).stderr
    lufs = re.findall(r"I:\s+(-?[\d.]+) LUFS", out)
    dur = re.search(r"Duration: (\d+):(\d+):([\d.]+)", out)
    seconds = int(dur.group(1)) * 3600 + int(dur.group(2)) * 60 + float(dur.group(3)) if dur else 0
    return seconds, float(lufs[-1]) if lufs else None


def logo_info(base, title):
    db = sqlite3.connect(base / "system_data/priv/mms/app.db")
    row = db.execute("SELECT m.logoImageInfo FROM tbl_contentinfo c JOIN tbl_conceptmetadata m "
                     "ON m.localConceptId = c.localConceptId WHERE c.titleId = ?", (title,)).fetchone()
    db.close()
    return row[0] if row else "no entry"


def dds_info(path):
    data = Path(path).read_bytes()
    return data[:148], len(data)


def pictures_tests(h, base, work):
    """The person's own pictures: the folder, mine: pictures in the picker's requests, and sent from a phone"""
    folder = base / "data/artswap/pictures"
    rows = h.rows("PICTURES")
    check(rows and rows[0] == ["folder", str(folder)] and not [r for r in rows if r[0] == "F"],
          f"PICTURES: the folder, empty at first ({rows[:2]})")
    # an icon with a transparent background, a photo, a PNG made see-through by tRNS, and what is not a picture
    Image.new("RGBA", (600, 400), (255, 255, 255, 0)).save(folder / "Logo art.png")
    Image.linear_gradient("L").resize((1600, 900)).convert("RGB").save(folder / "photo.jpg", quality=90)
    Image.new("RGB", (300, 300), (0, 0, 0)).save(folder / "keyed.png", transparency=(0, 0, 0))
    Image.new("RGB", (256, 256), (10, 200, 90)).save(folder / "plain.PNG")
    (folder / "notes.txt").write_text("not a picture")
    (folder / "fake.png").write_bytes(b"\x89PNG but not really")
    Image.new("RGB", (64, 64)).save(folder / ".hidden.png")
    (folder / ".upload.png.partial").write_bytes(b"half")
    now = time.time()
    for age, name in enumerate(("keyed.png", "photo.jpg", "plain.PNG", "Logo art.png")):
        os.utime(folder / name, (now - 100 * (age + 1), now - 100 * (age + 1)))
    found = [r for r in h.rows("PICTURES") if r[0] == "F"]
    check([r[1] for r in found] == ["keyed.png", "photo.jpg", "plain.PNG", "Logo art.png"],
          f"PICTURES: PNG and JPEG only, newest first ({[r[1] for r in found]})")
    info = {r[1]: r[2:] for r in found}
    check(info.get("photo.jpg", [""])[1:] == ["1600", "900", "0"], f"PICTURES: a JPEG's size, no alpha ({info.get('photo.jpg')})")
    check(info.get("Logo art.png", [""])[1:] == ["600", "400", "1"], "PICTURES: a PNG with an alpha channel")
    check(info.get("keyed.png", [""])[3] == "1" and info.get("plain.PNG", [""])[3] == "0",
          "PICTURES: tRNS counts as transparent, a plain RGB PNG does not")
    check(info.get("photo.jpg", ["0"])[0] == str((folder / "photo.jpg").stat().st_size), "PICTURES: the size in bytes")

    w, hh, _ = h.ask("THUMB", "mine:photo.jpg", "160", "160")
    check((w, hh) == (160, 90), f"THUMB takes one of the person's pictures ({w}x{hh})")
    w, hh, _ = h.ask("PREVIEW", "mine:photo.jpg", "icon", "cover", "0", "0", "512", "512")
    check((w, hh) == (512, 512), f"PREVIEW frames one as an icon ({w}x{hh})")
    for bad in ("mine:../apikey.txt", "mine:.hidden.png", "mine:missing.png", "mine:", "mine:a\\b.png"):
        try:
            h.ask("THUMB", bad, "64", "64")
            check(False, f"{bad} is refused")
        except HelperError as e:
            check(True, f"{bad} is refused ({e})")
    cache = base / "data/artswap/cache"
    before = len(list(cache.glob("*"))) if cache.exists() else 0
    icon = base / "user/appmeta/PPSA99314/icon0.png"
    report = h.rows("APPLY", "PPSA99314", "icon", "mine:photo.jpg", "cover", "0", "0")
    check(sum(r[0] == "W" for r in report) >= 2 and Image.open(icon).size == (512, 512),
          f"APPLY writes one of the person's pictures as the icon ({sum(r[0] == 'W' for r in report)} files)")
    check((len(list(cache.glob("*"))) if cache.exists() else 0) == before, "the person's pictures are never cached")
    report = h.rows("LOGO", "PPSA99314", "mine:Logo art.png")
    check(any(r[0] == "W" for r in report) and logo_info(base, "PPSA99314").startswith("/user/appmeta/PPSA99314/logo.png"),
          "LOGO takes a transparent PNG of the person's")
    report = h.rows("LOGO", "PPSA99314", "mine:photo.jpg")
    logo = Image.open(base / "user/appmeta/PPSA99314/logo.png")
    check(logo.format == "PNG", f"LOGO turns a JPEG of the person's into a PNG ({logo.format} {logo.size})")
    h.rows("RESTORE", "PPSA99314", "logo")
    h.rows("RESTORE", "PPSA99314", "icon")

    check(h.ask("PICDEL", "plain.PNG") == "" and not (folder / "plain.PNG").exists(), "PICDEL deletes a picture")
    check("plain.PNG" not in [r[1] for r in h.rows("PICTURES") if r[0] == "F"], "... and PICTURES no longer lists it")
    for bad in ("../apikey.txt", ".hidden.png", "missing.png"):
        try:
            h.ask("PICDEL", bad)
            check(False, f"PICDEL {bad} is refused")
        except HelperError as e:
            check(True, f"PICDEL {bad} is refused ({e})")
    check((base / "data/artswap/apikey.txt").exists() and (folder / ".hidden.png").exists(),
          "PICDEL leaves anything else alone")

    # sent from a phone
    rows = dict(r[:2] for r in h.rows("PHONE", "5151", "pictures"))
    check(rows.get("port") == str(PHONE_PORT), f"PHONE ... pictures opens the page ({rows})")
    page = f"http://127.0.0.1:{PHONE_PORT}"
    time.sleep(0.5)
    html = urllib.request.urlopen(page + "/5151", timeout=5).read().decode()
    check("Send pictures to your PS5" in html and "value='5151'" in html and "accept='image/*'" in html,
          "the pictures page carries the code")
    check("createImageBitmap" in html and "(sent as PNG)" in html and "/\\.png$/i" in html,
          "the pictures page turns other pictures into PNGs on the phone")
    (work / "shots/phone-pictures.html").write_text(html)

    def put(code, name, body):
        req = urllib.request.Request(f"{page}/upload?code={code}&name={urllib.parse.quote(name)}", data=body, method="PUT")
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.status, r.read().decode()
        except urllib.error.HTTPError as e:
            try:
                return e.code, e.read().decode()
            except OSError:  # answered before the body was read, and closed
                return e.code, ""

    import io
    png = io.BytesIO()
    Image.new("RGBA", (512, 512), (200, 40, 40, 128)).save(png, "PNG")
    png = png.getvalue()
    status, _ = put("5151", "From my phone.png", png)
    check(status == 200 and (folder / "From my phone.png").read_bytes() == png, f"a phone sends a picture ({status})")
    jpg = (folder / "photo.jpg").read_bytes()
    import http.client
    conn = http.client.HTTPConnection("127.0.0.1", PHONE_PORT, timeout=30)
    conn.putrequest("PUT", "/upload?code=5151&name=" + urllib.parse.quote("IMG_0001.JPG"))
    conn.putheader("Transfer-Encoding", "chunked")
    conn.endheaders()
    for at in range(0, len(jpg), 30000):
        piece = jpg[at:at + 30000]
        conn.send(b"%x\r\n" % len(piece) + piece + b"\r\n")
    conn.send(b"0\r\n\r\n")
    status = conn.getresponse().status
    conn.close()
    check(status == 200 and (folder / "IMG_0001.JPG").read_bytes() == jpg, f"a picture sent in chunks arrives whole ({status})")
    status, text = put("5151", "sneaky.png", b"<html>not a picture</html>")
    check(status == 400 and "PNG and JPEG" in text and not (folder / "sneaky.png").exists(),
          f"a file that is not a PNG or a JPEG is deleted ({status} {text})")
    status, _ = put("5151", "song.mp3", png)
    check(status == 400, "only .png, .jpg and .jpeg names")
    status, _ = put("5151", "../../apikey.png", png)
    check(status == 200 and (folder / "apikey.png").exists() and not (base / "apikey.png").exists(),
          "a name with folders in it lands in the pictures folder")
    check(put("1111", "x.png", png)[0] == 403, "a wrong code sends no picture")
    conn = http.client.HTTPConnection("127.0.0.1", PHONE_PORT, timeout=30)
    conn.putrequest("PUT", "/upload?code=5151&name=huge.png")
    conn.putheader("Content-Length", str(41 << 20))
    conn.endheaders()
    response = conn.getresponse()
    check(response.status == 413 and "40 MB" in response.read().decode(), f"a picture over 40 MB is refused ({response.status})")
    conn.close()
    check(not list(folder.glob(".*partial")) or [p.name for p in folder.glob(".*partial")] == [".upload.png.partial"],
          "no partial picture is left")
    names = [r[1] for r in h.rows("PICTURES") if r[0] == "F"]
    check(names[:2] == ["apikey.png", "IMG_0001.JPG"] or set(names[:3]) >= {"apikey.png", "IMG_0001.JPG"},
          f"what a phone sent comes first ({names[:4]})")
    try:
        urllib.request.urlopen(page + "/upload?code=5151&name=x.png", data=b"x", timeout=5)
        check(False, "a POST sends nothing to the pictures page")
    except urllib.error.HTTPError as e:
        check(e.code == 404, f"a POST sends nothing to the pictures page ({e.code})")
    h.ask("PHONE")
    time.sleep(1.0)


# ---- the home screen's layout (layout.c), on a console of its own ----

LAYOUT_PORT = 19317
U1, U2 = "0349465199", "0123456789"
TIME_COLUMNS = ("lastAccessTime", "lastPlayedDate", "lastPlayedDateOnConsole", "recentActivityDatePlayedOrPurchased",
                "recentActivityDatePlayedOrInstalled")


def make_layout_db(path):
    """A home-screen database with the layout's tables, as 13.60's: two users (the second with fewer tiles), games,
    a media tool, the system's tiles (two in MyHome, one hidden, a disc placeholder) and an uninstalled leftover"""
    db = sqlite3.connect(path)
    for user in (U1, U2):
        db.execute(f"CREATE TABLE tbl_iconinfo_{user} (titleId TEXT NOT NULL, titleName TEXT COLLATE NOCASE, "
                   "localConceptId TEXT, lastAccessTime DATETIME, lastAccessIndex INT, lastPlayedDate DATETIME, "
                   "lastPlayedIndex INT, lastPlayedDateOnConsole DATETIME, recentActivityDatePlayedOrPurchased DATETIME, "
                   "recentActivityDatePlayedOrInstalled DATETIME, installStatus INT, dispLocation INT, visible INT, "
                   "PRIMARY KEY(titleId))")
        db.execute(f"CREATE TABLE tbl_concepticoninfo_{user} (localConceptId TEXT NOT NULL, conceptName TEXT, "
                   "lastAccessIndex INT, lastPlayedDate DATETIME, lastPlayedDateOnConsole DATETIME, "
                   "lastInteractedTime DATETIME, dispLocation INT, PRIMARY KEY(localConceptId))")
        db.execute(f"CREATE TABLE tbl_group_{user} (itemId TEXT NOT NULL, groupName TEXT NOT NULL, type TEXT NOT NULL, "
                   "idx INT, lastModifiedDate TEXT, PRIMARY KEY(itemId, groupName))")
        db.execute(f"INSERT INTO tbl_group_{user} VALUES ('NPXS40047', 'MyHome', 'title', 1, NULL)")
        db.execute(f"INSERT INTO tbl_group_{user} VALUES ('NPXS40071', 'MyHome', 'title', 2, NULL)")
        db.execute(f"INSERT INTO tbl_group_{user} VALUES ('NPXS40071', 'Other', 'title', 1, NULL)")
    db.execute("CREATE TABLE tbl_contentinfo (titleId TEXT, titleName TEXT, localConceptId TEXT, lastAccessTime DATETIME, "
               "lastAccessIndex INT, dispLocation INT, installStatus INT)")
    db.execute("CREATE TABLE tbl_conceptmetadata (localConceptId TEXT NOT NULL, conceptName TEXT, lastAccessTime DATETIME, "
               "lastAccessIndex INT, dispLocation INT, installStatus INT, PRIMARY KEY(localConceptId))")
    db.execute("CREATE TABLE tbl_version (category TEXT, status INT)")
    db.executemany("INSERT INTO tbl_version VALUES (?, ?)", [("access_index", 900), ("played_index", 300)])
    # id, title name, concept name, index, played, install, dispLocation, visible
    tiles = [("PPSA00001", "game one (old)", "Game One", 880, None, 2, 138, 1),
             ("PPSA00002", "Game Two", "Game Two", 870, None, 2, 138, 1),
             ("PPSA00003", "Game Three", "Game Three", 860, "2026-10-20 10:00:00.000", 2, 138, 1),
             ("ORBT00001", "Orbit Store", "Orbit Store", 850, None, 2, 188, 1),
             ("PPSA00009", "Gone", "Gone", 840, None, 0, 138, 0),
             ("NPXS40047", "PlayStation Store", "PlayStation Store", 2, None, 2, 146, 1),
             ("NPXS40071", "Game Library", "Game Library", 4, None, 2, 146, 1),
             ("NPXS40053", "Discover", "Discover", 0, None, 2, 149, 0),
             ("NPXS40140", "Disc Player", "Disc Player", 0, None, 2, 157, 0)]
    for i, (tid, name, concept_name, index, played, install, disp, visible) in enumerate(tiles):
        local = f"cid:local:{tid}"
        when = f"2026-10-2{i % 3} 1{i}:00:00.000"
        for user in (U1, U2):
            if user == U2 and tid == "PPSA00003":
                continue
            db.execute(f"INSERT INTO tbl_iconinfo_{user} VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                       (tid, name, local, when, index, played, 690 if played else None, played, played, when, install,
                        disp, visible))
            db.execute(f"INSERT INTO tbl_concepticoninfo_{user} VALUES (?, ?, ?, ?, ?, ?, ?)",
                       (local, concept_name, index, played, played, when, disp + 2048))
        db.execute("INSERT INTO tbl_contentinfo VALUES (?, ?, ?, ?, ?, ?, ?)", (tid, name, local, when, index, disp, install))
        db.execute("INSERT INTO tbl_conceptmetadata VALUES (?, ?, ?, ?, ?, ?)", (local, concept_name, when, index, disp, install))
    db.commit()
    db.close()


def home_restart(h, what):
    """Apply changes, with Art Swap already closed; the HOMESTATE line at the end"""
    title = subprocess.Popen(["true"])
    title.wait()
    h.rows("HOME", str(title.pid))
    end = time.time() + 15
    state = None
    while time.time() < end:
        state = next(r for r in h.rows("HOMESTATE") if r[0] == "home")
        if state[1] in ("done", "failed"):
            break
        time.sleep(0.2)
    check(state is not None and state[1] == "done", f"{what}: Apply changes finishes ({state})")
    h.rows("HOMESTATE", "seen")
    return state


def table_rows(path, table, key):
    db = sqlite3.connect(path)
    db.row_factory = sqlite3.Row
    rows = {r[key]: dict(r) for r in db.execute(f"SELECT * FROM \"{table}\"")}
    db.close()
    return rows


def run_layout_helper(base, env_extra=None):
    env = dict(os.environ, ARTSWAP_ROOT=str(base), ARTSWAP_PORT=str(LAYOUT_PORT), ARTSWAP_PHONE_PORT="19318",
               ARTSWAP_LAN="127.0.0.1", ARTSWAP_SMP_PORT="19301")
    log = open(base.parent / f"{base.name}.out", "w")
    proc = subprocess.Popen([str(ROOT / "build/helper-host/artswapd")], env=env, stdout=log, stderr=subprocess.STDOUT)
    check(wait_port(LAYOUT_PORT), f"the helper listens on {base.name}")
    return proc, log, Helper(port=LAYOUT_PORT)


def layout_tests(work):
    base = work / "layout-console"
    db_path = base / "system_data/priv/mms/app.db"
    db_path.parent.mkdir(parents=True)
    (base / "data/artswap").mkdir(parents=True)
    make_layout_db(db_path)
    proc, log, h = run_layout_helper(base)
    try:
        rows = h.rows("LAYOUT")
        check(rows[0] == ["order", "none"], f"LAYOUT: no order of Art Swap's ({rows[0]})")
        tiles = [r for r in rows if r[0] == "T"]
        check([r[1] for r in tiles] == ["PPSA00001", "PPSA00002", "PPSA00003", "ORBT00001", "NPXS40047", "NPXS40071",
                                        "NPXS40053"],
              f"LAYOUT: the games by recency, then the system's tiles, MyHome first; no disc placeholder or leftover "
              f"({[r[1] for r in tiles]})")
        by_id = {r[1]: r for r in tiles}
        check(by_id["PPSA00001"][2:] == ["games", "games", "game", "Game One"], f"LAYOUT: a game, by its concept's name ({by_id['PPSA00001']})")
        check(by_id["ORBT00001"][2:4] == ["media", "media"], "LAYOUT: a tool on the Media row")
        check(by_id["NPXS40047"][2:5] == ["games", "games", "system"] and by_id["NPXS40053"][2:4] == ["hidden", "hidden"],
              "LAYOUT: a system tile in MyHome is on Games; one not shown is hidden")

        answer = h.rows("PLACE", "PPSA00002", "media")
        check(answer == [["T", "PPSA00002", "media", "games", "game", "Game Two"]],
              f"PLACE: the wish, where != now until Apply changes ({answer})")
        h.rows("PLACE", "ORBT00001", "hidden")
        h.rows("PLACE", "NPXS40053", "games")
        h.rows("PLACE", "NPXS40047", "media")
        for bad, why in ((("PPSA12345", "games"), "no such tile"), (("NPXS40140", "games"), "no such tile"),
                         (("PPSA00001", "sideways"), "games, media or hidden")):
            try:
                h.ask("PLACE", *bad)
                check(False, f"PLACE {bad} is refused")
            except HelperError as e:
                check(why in str(e), f"PLACE {bad} is refused ({e})")
        h.rows("PLACE", "PPSA00003", "media")
        h.rows("PLACE", "PPSA00003", "games")
        layout_txt = (base / "data/artswap/layout.txt").read_text()
        check("PPSA00003" not in layout_txt, "PLACE back where it is clears the wish")
        pend = sorted(tuple(r[1:]) for r in h.rows("PENDING") if r[0] == "P")
        check(pend == [("NPXS40047", "layout"), ("NPXS40053", "layout"), ("ORBT00001", "layout"), ("PPSA00002", "layout")],
              f"PENDING: each tile moved waits for Apply changes ({pend})")

        ids = ["PPSA00003", "PPSA00001"] + [f"FAKE{i:05d}" for i in range(148)]
        answer = h.rows("ORDER", "locked", *ids)
        check(answer == [["order", "locked"]], f"ORDER locked with {len(ids)} ids ({answer})")
        check("\t".join(["seq"] + ids) in (base / "data/artswap/layout.txt").read_text(), "every id of the order is kept")
        games = [r[1] for r in h.rows("LAYOUT") if r[0] == "T" and r[4] == "game"]
        check(games[:2] == ["PPSA00003", "PPSA00001"] and h.rows("LAYOUT")[0] == ["order", "locked"],
              f"LAYOUT: the order wished, locked ({games})")
        check(("order", "layout") in [tuple(r[1:]) for r in h.rows("PENDING") if r[0] == "P"], "PENDING: the order waits")
        try:
            h.ask("ORDER", "sideways")
            check(False, "ORDER with an unknown mode is refused")
        except HelperError as e:
            check(True, f"ORDER with an unknown mode is refused ({e})")

        home_restart(h, "the layout")
        check((base / "data/artswap/app.db.backup").exists(), "the database is copied aside before the layout is written")
        db = sqlite3.connect(db_path)
        check(db.execute("PRAGMA integrity_check").fetchone()[0] == "ok", "the database passes its check")
        db.close()
        for user in (U1, U2):
            icon = table_rows(db_path, f"tbl_iconinfo_{user}", "titleId")
            concept = table_rows(db_path, f"tbl_concepticoninfo_{user}", "localConceptId")
            group = [r[0] for r in sqlite3.connect(db_path).execute(
                f"SELECT itemId FROM tbl_group_{user} WHERE groupName = 'MyHome' ORDER BY idx")]
            check(icon["PPSA00002"]["dispLocation"] == 140 and icon["PPSA00002"]["visible"] == 1 and
                  concept["cid:local:PPSA00002"]["dispLocation"] == 140 + 2048,
                  f"user {user}: a game moved to Media (the Games bit off, the Media bit on, nothing else)")
            check(icon["ORBT00001"]["visible"] == 0 and icon["ORBT00001"]["dispLocation"] == 188,
                  f"user {user}: a tool hidden, its row left as it was")
            check(icon["NPXS40053"]["visible"] == 1 and icon["NPXS40053"]["dispLocation"] & 0x06 == 0x02,
                  f"user {user}: a hidden system tile shown on Games")
            check(group == ["NPXS40053", "NPXS40071"],
                  f"user {user}: MyHome: the Store out, Discover in before Game Library ({group})")
            check(icon["NPXS40047"]["dispLocation"] & 0x06 == 0x04, f"user {user}: the Store on the Media row")
            if user == U1:
                check(icon["PPSA00003"]["lastAccessIndex"] == 1000000000 and
                      icon["PPSA00003"]["lastAccessTime"] == "2099-12-31 12:00:00.000" and
                      icon["PPSA00003"]["lastPlayedDate"] == "2099-12-31 12:00:00.000",
                      f"locked: the first game far in the future ({icon['PPSA00003']['lastAccessIndex']})")
            check(icon["PPSA00001"]["lastAccessIndex"] == 999999999 and
                  icon["PPSA00001"]["lastAccessTime"] == "2099-12-31 11:59:00.000" and
                  icon["PPSA00001"]["lastPlayedDate"] is None and icon["PPSA00001"]["lastPlayedIndex"] is None,
                  f"user {user}: the second a minute before; a game never played stays never played")
            check(icon["PPSA00002"]["lastAccessIndex"] == 870, f"user {user}: a game moved to Media keeps its recency")
            check(group == ["NPXS40053", "NPXS40071"] and sqlite3.connect(db_path).execute(
                f"SELECT count(*) FROM tbl_group_{user} WHERE groupName = 'Other'").fetchone()[0] == 1,
                f"user {user}: other groups left alone")
        content = table_rows(db_path, "tbl_contentinfo", "titleId")
        meta = table_rows(db_path, "tbl_conceptmetadata", "localConceptId")
        check(content["PPSA00002"]["dispLocation"] == 140 and meta["cid:local:PPSA00002"]["dispLocation"] == 140 and
              content["PPSA00003"]["lastAccessIndex"] == 1000000000 and
              meta["cid:local:PPSA00001"]["lastAccessTime"] == "2099-12-31 11:59:00.000",
              "tbl_contentinfo and tbl_conceptmetadata too")
        version = dict(sqlite3.connect(db_path).execute("SELECT category, status FROM tbl_version").fetchall())
        check(version == {"access_index": 900, "played_index": 300}, f"locked: the console's counters untouched ({version})")
        check(not [r for r in h.rows("PENDING") if r[0] == "P"], "nothing waits after Apply changes")
        check("place\tPPSA00002\tmedia\tgames" in (base / "data/artswap/layout.txt").read_text(),
              "the wishes are kept, for the next Apply changes")
        tiles = {r[1]: r for r in h.rows("LAYOUT") if r[0] == "T"}
        check(tiles["PPSA00002"][2:4] == ["media", "media"] and tiles["NPXS40047"][2:4] == ["media", "media"],
              "LAYOUT: where == now once written")

        # something put a tile back (a firmware update): the next Apply changes puts it right
        db = sqlite3.connect(db_path)
        db.execute(f"UPDATE tbl_iconinfo_{U1} SET dispLocation = 138 WHERE titleId = 'PPSA00002'")
        db.execute(f"UPDATE tbl_iconinfo_{U1} SET lastAccessIndex = 901 WHERE titleId = 'PPSA00001'")
        db.commit()
        db.close()
        pend = sorted(tuple(r[1:]) for r in h.rows("PENDING") if r[0] == "P")
        check(pend == [("PPSA00002", "layout"), ("order", "layout")], f"PENDING: a tile put back, a locked game played ({pend})")
        home_restart(h, "the layout again")
        icon = table_rows(db_path, f"tbl_iconinfo_{U1}", "titleId")
        check(icon["PPSA00002"]["dispLocation"] == 140 and icon["PPSA00001"]["lastAccessIndex"] == 999999999,
              "Apply changes writes the layout again")

        # unlocked: written once in the order it has, now, with the console's counters raised
        check(h.rows("ORDER", "none") == [["order", "free"]], "ORDER none after locked: written once more, free")
        check(("order", "layout") in [tuple(r[1:]) for r in h.rows("PENDING") if r[0] == "P"], "PENDING: the order waits")
        home_restart(h, "the order freed")
        icon = table_rows(db_path, f"tbl_iconinfo_{U1}", "titleId")
        version = dict(sqlite3.connect(db_path).execute("SELECT category, status FROM tbl_version").fetchall())
        games = 2  # on the Games row: Game Three and Game One (Game Two is on Media)
        check(version == {"access_index": 900 + games, "played_index": 300 + games},
              f"free: the console's counters go on from the new order ({version})")
        check(icon["PPSA00003"]["lastAccessIndex"] == 902 and icon["PPSA00001"]["lastAccessIndex"] == 901 and
              icon["PPSA00003"]["lastAccessTime"].startswith(time.strftime("%Y-", time.gmtime())),
              f"free: the same order, as now ({icon['PPSA00003']['lastAccessIndex']}, {icon['PPSA00001']['lastAccessIndex']})")
        check(h.rows("LAYOUT")[0] == ["order", "none"] and "dirty" not in (base / "data/artswap/layout.txt").read_text() and
              "order\tnone" in (base / "data/artswap/layout.txt").read_text(), "free goes back to none once written")
        check(not [r for r in h.rows("PENDING") if r[0] == "P"], "nothing waits")

        # back where it began: kept until written, then forgotten
        h.rows("PLACE", "PPSA00002", "games")
        check(("PPSA00002", "layout") in [tuple(r[1:]) for r in h.rows("PENDING") if r[0] == "P"],
              "PLACE back to where it began waits until the home screen shows it")
        home_restart(h, "a tile put back")
        check(table_rows(db_path, f"tbl_iconinfo_{U1}", "titleId")["PPSA00002"]["dispLocation"] == 138 and
              "PPSA00002" not in (base / "data/artswap/layout.txt").read_text(),
              "... then the wish is forgotten")
        h.ask("BYE")
        proc.wait(timeout=10)
    finally:
        if proc.poll() is None:
            proc.kill()
        log.close()

    # a console's own database (ARTSWAP_REAL_APPDB, a copy of /system_data/priv/mms/app.db; 13.60's was used):
    # only the layout's cells change
    real = Path(os.environ.get("ARTSWAP_REAL_APPDB", ""))
    if not os.environ.get("ARTSWAP_REAL_APPDB") or not real.is_file():
        print("skip the real database: set ARTSWAP_REAL_APPDB to a copy of a console's app.db")
        return
    base = work / "layout-real"
    db_path = base / "system_data/priv/mms/app.db"
    db_path.parent.mkdir(parents=True)
    (base / "data/artswap").mkdir(parents=True)
    shutil.copy(real, db_path)
    before = sqlite3.connect(db_path)
    tables = [r[0] for r in before.execute("SELECT name FROM sqlite_master WHERE type = 'table'")]
    snapshot = {t: before.execute(f"SELECT rowid, * FROM \"{t}\"").fetchall() for t in tables}
    columns = {t: ["rowid"] + [r[1] for r in before.execute(f"PRAGMA table_info(\"{t}\")")] for t in tables}
    before.close()
    proc, log, h = run_layout_helper(base)
    try:
        tiles = [r for r in h.rows("LAYOUT") if r[0] == "T"]
        check(len(tiles) == 48, f"the real database: LAYOUT lists its tiles ({len(tiles)})")
        h.rows("PLACE", "PPSA99318", "media")
        h.rows("PLACE", "NPXS40047", "hidden")
        h.rows("ORDER", "locked", "PPSA99000", "PPSA01325")
        home_restart(h, "the real database")
        h.ask("BYE")
        proc.wait(timeout=10)
    finally:
        if proc.poll() is None:
            proc.kill()
        log.close()
    after = sqlite3.connect(db_path)
    check(after.execute("PRAGMA integrity_check").fetchone()[0] == "ok", "the real database passes its check")
    allowed = {"dispLocation", "visible", "lastAccessIndex", "lastPlayedIndex", "lastInteractedTime", *TIME_COLUMNS}
    changed = {}
    for t in tables:
        now = after.execute(f"SELECT rowid, * FROM \"{t}\"").fetchall()
        if t.startswith("tbl_group_"):
            others = lambda rows: sorted(r for r in rows if r[2] != "MyHome")  # noqa: E731
            if others(now) != others(snapshot[t]):
                changed.setdefault(t, set()).add("groups other than MyHome")
            continue
        old = {r[0]: r for r in snapshot[t]}
        if len(now) != len(old):
            changed.setdefault(t, set()).add("rows added or removed")
        for row in now:
            was = old.get(row[0])
            for i, column in enumerate(columns[t]):
                if was is None or was[i] != row[i]:
                    changed.setdefault(t, set()).add(column)
    myhome = [r[0] for r in after.execute(f"SELECT itemId FROM tbl_group_{U1} WHERE groupName = 'MyHome' ORDER BY idx")]
    after.close()
    stray = {t: c - allowed for t, c in changed.items() if c - allowed}
    check(not stray, f"the real database: only the layout's cells change ({stray or sorted(set().union(*changed.values()))})")
    check(myhome == ["NPXS40037", "NPXS40071"], f"the real database: the Store taken out of MyHome ({myhome})")
    icon = table_rows(db_path, f"tbl_iconinfo_{U1}", "titleId")
    check(icon["PPSA99318"]["dispLocation"] == 140 and icon["PPSA99000"]["lastAccessIndex"] == 1000000000 and
          icon["PPSA01325"]["lastAccessIndex"] == 999999999 and icon["PPSA99203"]["lastAccessIndex"] == 999999998,
          "the real database: Art Swap on Media, the Games row locked from ProsperoStore")


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
    # the titles' download data (/download0), where some homebrew keeps its saves; the uninstall deletes it
    download = base / "user/download/PPSA99314/download0.dat"
    psp_download = base / "user/download/PPSA99131/download0.dat"
    for path in (download, psp_download):
        path.parent.mkdir(parents=True)
        path.write_bytes(os.urandom(1 << 20))
    download_sha, psp_download_sha = sha(download), sha(psp_download)
    helper_bin = ROOT / "build/helper-host/artswapd"
    log = open(work / "helper.out", "w")
    proc = subprocess.Popen([str(helper_bin)], env=env, stdout=log, stderr=subprocess.STDOUT)
    try:
        check(wait_port(PORT), "the helper listens")
        h = Helper(port=PORT)

        hello = dict(r[:2] for r in h.rows("HELLO"))
        check(hello.get("artswapd") == "0.6.0", f"HELLO names the version ({hello})")
        check(hello.get("store") == "off", "PlayStation Store logos start off")
        check(hello.get("key") == "user", "the person's key (apikey.txt) is in")
        check(hello.get("lan") == "127.0.0.1", "HELLO gives the console's address")

        apps = {r[1]: r for r in h.rows("APPS")}
        print("     " + "\n     ".join("\t".join(r) for r in apps.values()))
        check(set(apps) == {"PPSA99314", "CUSA00001", "PPSA05678", "PPSA99316", "PPSA26525", "PPSA99131"},
              "APPS lists the six titles, not the system app")
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
        check(ed[11:14] == ["0", "-", "1"], f"APPS: homebrew on the home screen, no store concept, no logo ({ed[11:14]})")
        check(apps["PPSA26525"][11] == "10012903", "APPS: a store game's concept")
        check(apps["PPSA99316"][13] == "0", "APPS: a title the home screen does not list")
        check(apps["PPSA99314"][15] == "314" and apps["PPSA99316"][15] == "0", "APPS: the home screen's order")
        check(h.rows("PREF", "sort")[0] == ["pref", "sort", ""], "PREF: nothing chosen yet")
        h.rows("PREF", "sort", "home screen")
        check(h.rows("PREF", "sort")[0][2] == "home screen", "PREF keeps a choice")
        try:
            h.rows("PREF", "so rt", "x")
            check(False, "PREF refuses a key that is not letters and digits")
        except HelperError:
            check(True, "PREF refuses a key that is not letters and digits")
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

        # a logo on the game's home-screen page: the PNG beside its art, the database pointed at it
        logo = lists["logos"][0]
        report = h.rows("LOGO", "PPSA99314", logo[2])
        logo_png = base / "user/appmeta/PPSA99314/logo.png"
        check(any(r[0] == "W" and r[1] == "/user/appmeta/PPSA99314/logo.png" for r in report), f"LOGO writes logo.png ({report})")
        check(logo_png.exists() and Image.open(logo_png).format == "PNG", "logo.png is a PNG")
        check(logo_info(base, "PPSA99314").startswith("/user/appmeta/PPSA99314/logo.png?ts="),
              f"logoImageInfo points at it ({logo_info(base, 'PPSA99314')})")
        check((base / "data/artswap/logos/PPSA99314.png").exists(), "Art Swap keeps its own copy")
        check((base / "data/artswap/app.db.first").exists() and (base / "data/artswap/app.db.backup").exists(),
              "the database is copied aside before it changes")
        first = sqlite3.connect(base / "data/artswap/app.db.first")
        check(first.execute("SELECT count(*) FROM tbl_conceptmetadata WHERE logoImageInfo IS NOT NULL").fetchone()[0] == 0,
              "app.db.first is the database from before")
        first.close()
        w, hh, rgba = h.ask("ART", "PPSA99314", "logo", 600, 300)
        check(w <= 600 and hh <= 300, f"ART logo ({w}x{hh})")
        check({r[1]: r for r in h.rows("APPS")}["PPSA99314"][12] == "art", "APPS: Art Swap's logo")
        try:
            h.rows("LOGO", "PPSA99316", logo[2])
            check(False, "LOGO refuses a title the home screen does not list")
        except HelperError as e:
            check("no entry" in str(e), f"LOGO refuses a title the home screen does not list ({e})")

        # PlayStation Store logos: off until turned on; then the catalog's own, for a store game only
        try:
            h.rows("STORELOGO", "PPSA26525")
            check(False, "STORELOGO is off at first")
        except HelperError as e:
            check("off" in str(e), f"STORELOGO is off at first ({e})")
        check(h.rows("STORE", "on")[0][1] == "on" and (base / "data/artswap/store-logos").exists(), "STORE on")
        store = [r for r in h.rows("STORELOGO", "PPSA26525") if r[0] == "I"]
        check(len(store) == 1 and store[0][2].startswith("https://image.api.playstation.com/") and
              int(store[0][4]) > 0 and store[0][7] == "PlayStation Store", f"STORELOGO: the store's logo ({store})")
        check((base / "data/artswap/store/10012903.txt").exists(), "the store's answer is kept")
        try:
            h.rows("STORELOGO", "PPSA99314")
            check(False, "STORELOGO refuses homebrew")
        except HelperError as e:
            check("not a PlayStation Store game" in str(e), f"STORELOGO refuses homebrew ({e})")
        w, hh, rgba = h.ask("THUMB", store[0][2], 400, 200)
        check(w > 0, "THUMB shows the store's logo")
        h.rows("LOGO", "PPSA26525", store[0][2])
        check(logo_info(base, "PPSA26525").startswith("/user/appmeta/PPSA26525/logo.png?ts="), "LOGO with the store's logo")
        report = h.rows("RESTORE", "PPSA26525", "logo")
        check(logo_info(base, "PPSA26525") is None and not (base / "user/appmeta/PPSA26525/logo.png").exists(),
              f"RESTORE logo takes it away, the page's own (none) back ({report})")
        check(h.rows("STORE", "off")[0][1] == "off", "STORE off")
        try:
            h.ask("THUMB", store[0][2], 400, 200)
            check(False, "with store logos off, Sony's pictures are refused")
        except HelperError as e:
            check("off" in str(e), f"with store logos off, Sony's pictures are refused ({e})")

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
        check(logo_png.exists() and logo_info(base, "PPSA99314").startswith("/user/appmeta/PPSA99314/logo.png?ts="),
              f"the install took the logo away; it is put back ({logo_info(base, 'PPSA99314')})")
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
        check(sha(download) == download_sha and not (base / "user/download/PPSA99314.artswap").exists(),
              "the download data the uninstall deletes is back in its place, as it was")
        check((base / "data/artswap/download-aside.txt").read_text() == "", "download-aside.txt is empty again")
        h.rows("REFRESHSTATE", "seen")
        check(refresh_state()[3] == "0", "REFRESHSTATE seen: shown")
        check("\t0\t" in (base / "data/artswap/refresh.txt").read_text(), "refresh.txt keeps it as seen")

        # download data left aside by an earlier run: the refresh changes nothing
        stale = base / "user/download/PPSA99314.artswap"
        stale.mkdir()
        uninstalls = smp.console.calls.count("/api/v1/games/uninstall")
        time.sleep(1.1)
        h.rows("REFRESH", "PPSA99314", "0")
        state = refresh_until_end()
        check(state[1] == "failed" and "download data safe" in state[4] and
              smp.console.calls.count("/api/v1/games/uninstall") == uninstalls and sha(download) == download_sha,
              f"an earlier copy of the download data stops the refresh before the uninstall ({state[4]})")
        stale.rmdir()
        h.rows("REFRESHSTATE", "seen")

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

        installed_param = base / "user/app/PPSA99314/sce_sys/param.json"  # the refreshes' install made it
        report = h.rows("RENAME", "PPSA99314", "Eternal Darkness: Sanity's Requiem")
        check(sum(r[0] == "W" for r in report) == 3 and sum(r[0] == "O" for r in report) == 3,
              f"RENAME writes every param.json (the folder's, the home screen's, the install's) and keeps each "
              f"original ({report})")
        check(names(folder_param) == names(meta_param) == names(installed_param) == {"Eternal Darkness: Sanity's Requiem"},
              "the new name is in all three")
        # quotes, a slash and an accent: escaped in the database's JSON as the console escapes them
        new_name = 'Eternal Darkness é / "Requiem"'
        h.rows("RENAME", "PPSA99314", new_name)
        check(Path(str(folder_param) + ".old").read_text() == original_param, "a second rename keeps the first original")
        apps = {r[1]: r for r in h.rows("APPS")}
        check(apps["PPSA99314"][2] == new_name and apps["PPSA99314"][9] == "1",
              f"APPS: the new name, its original kept ({apps['PPSA99314']})")
        for title, name, why in (("CUSA00001", "Bloodborne 2", "package"), ("PPSA99314", "a\x01b", "character")):
            try:
                h.rows("RENAME", title, name)
                check(False, f"RENAME refuses {why}")
            except HelperError as e:
                check(why in str(e), f"RENAME refuses: {e}")
        # Apply changes writes the new name into the home screen's database, with no install, then restarts it
        pend = {r[1]: r[2] for r in h.rows("PENDING") if r[0] == "P"}
        check(pend.get("PPSA99314") == "name", f"PENDING: the rename waits for Apply changes ({pend})")
        _, _, info_before, concept_before = home_names(base, "PPSA99314")
        # a rename the list lost (made before renames waited for Apply changes): found again by its name
        (base / "data/artswap/pending.txt").write_text("")
        h.rows("APPS", "rescan")
        pend = {r[1]: r[2] for r in h.rows("PENDING") if r[0] == "P"}
        check(pend.get("PPSA99314") == "name", f"a rename the home screen does not show yet waits again ({pend})")
        marker.unlink(missing_ok=True)
        uninstalls = smp.console.calls.count("/api/v1/games/uninstall")
        title = subprocess.Popen(["sleep", "1"])  # Art Swap, closing
        h.rows("HOME", str(title.pid))
        title.wait()
        end = time.time() + 90
        while time.time() < end and next(r for r in h.rows("HOMESTATE") if r[0] == "home")[1] not in ("done", "failed"):
            time.sleep(0.3)
        home = next(r for r in h.rows("HOMESTATE") if r[0] == "home")
        check(home[1] == "done" and marker.exists(), f"Apply changes after a rename is done ({home})")
        check(smp.console.calls.count("/api/v1/games/uninstall") == uninstalls,
              "a rename installs nothing again: ShadowMountPlus is not asked to uninstall")
        check(sha(download) == download_sha, "the renamed game's download data is untouched")
        held, sizes, info_after, concept_after = home_names(base, "PPSA99314")
        check(held == {new_name}, f"the home screen's database has the new name in every place ({held})")
        check(sizes, "each concept name's length in bytes is the new name's")
        check(without_names(info_after, concept_after) == without_names(info_before, concept_before),
              "nothing else in the entry's JSON changed (its 64-bit numbers neither)")
        check("1396115914549624833" in info_after and "\\/user\\/app" in info_after and
              'Darkness é \\/ \\"Requiem\\"' in info_after,
              "the JSON is written as the console writes it: UTF-8 as it is, / and \" escaped")
        check(names(meta_param) == {new_name}, "the home screen's copy of param.json has the new name")
        check(logo_png.exists() and logo_info(base, "PPSA99314").startswith("/user/appmeta/PPSA99314/logo.png?ts="),
              "a renamed title keeps its logo through Apply changes")
        check(Path(str(meta_param) + ".old").exists(), "the home screen's original name is kept")
        check(not [r for r in h.rows("PENDING") if r[0] == "P"], "nothing is waiting after Apply changes")
        h.rows("APPS", "rescan")
        check(not [r for r in h.rows("PENDING") if r[0] == "P"], "the home screen shows the new name: nothing waits")
        h.rows("HOMESTATE", "seen")

        # restore; a copy without its .old takes the folder's original
        Path(str(base / "user/appmeta/PPSA99314/pic0.dds") + ".old").unlink()
        # the install copied the folder's sce_sys, its .old files too, into the home screen's copy; a scan
        # finds that copy, and Restore puts its originals back as well, for a restart to show them
        h.rows("APPS", "rescan")
        installed_olds = len(list((base / "user/app/PPSA99314/sce_sys").glob("*.old")))
        check(installed_olds == 4, f"the installed copy has the folder's originals ({installed_olds})")
        report = h.rows("RESTORE", "PPSA99314", "all")
        check(sum(r[0] == "R" for r in report) == 8 + installed_olds + 1,
              f"RESTORE puts six art files, two names, the installed copy and the logo back ({len(report)})")
        check(not logo_png.exists() and logo_info(base, "PPSA99314") is None and
              not (base / "data/artswap/logos/PPSA99314.png").exists(), "RESTORE all takes the logo away")
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

        # a background for a folder game that has none: pic0.dds in its folder, taken in by an install
        psp = base / "data/homebrew/PPSA99131/sce_sys/pic0.dds"
        check({r[1]: r for r in h.rows("APPS")}["PPSA99131"][5] == "-", "PSP5 has no background")
        try:
            h.rows("APPLY", "PPSA05678", "pic", hero[2], "cover", "0", "0")
            check(False, "no background is added to a game ShadowMountPlus cannot install again")
        except HelperError as e:
            check("installed from a folder" in str(e), f"no background is added to a game it cannot install ({e})")
        report = h.rows("APPLY", "PPSA99131", "pic", hero[2], "cover", "0", "0")
        im = Image.open(psp)
        im.load()
        check(im.size == (3840, 2160) and dds_info(psp)[1] == 8294548,
              f"APPLY adds pic0.dds, 3840x2160 BC7, to the folder ({report})")
        # Astro's Playroom's pic0.dds on the console, as the system wrote it
        astro_header = bytes.fromhex(
            "444453207c00000007100a0070080000000f000000907e000000000001000000000000000000000000000000000000000000"
            "000000000000000000000000000000000000000000000000000020000000040000004458313000000000000000000000000000"
            "0000000000000000100000000000000000000000000000000000006200000003000000000000000100000000000000")
        check(dds_info(psp)[0] == astro_header, "its header is the one the console writes for its own")
        check(not Path(str(psp) + ".old").exists(), "an added background has no original")
        check({r[1]: r for r in h.rows("APPS")}["PPSA99131"][14] == "1", "APPS: a background Art Swap added")
        pend = {r[1]: r[2] for r in h.rows("PENDING") if r[0] == "P"}
        check(pend.get("PPSA99131") == "install", f"an added background waits for an install ({pend})")
        check(pend.get("PPSA99314") == "name", f"the restored name waits for Apply changes ({pend})")
        uninstalls = smp.console.calls.count("/api/v1/games/uninstall")
        title = subprocess.Popen(["sleep", "1"])
        h.rows("HOME", str(title.pid))
        title.wait()
        end = time.time() + 90
        while time.time() < end and next(r for r in h.rows("HOMESTATE") if r[0] == "home")[1] not in ("done", "failed"):
            time.sleep(0.3)
        installed = base / "user/appmeta/PPSA99131/pic0.dds"
        end = time.time() + 5  # the fake install copies its files in no fixed order
        while time.time() < end and not installed.exists():
            time.sleep(0.2)
        check(next(r for r in h.rows("HOMESTATE") if r[0] == "home")[1] == "done" and installed.exists(),
              "the install takes the added background in")
        check(smp.console.calls.count("/api/v1/games/uninstall") == uninstalls + 1,
              "only the game with the added background was installed again")
        check(sha(psp_download) == psp_download_sha and not (base / "user/download/PPSA99131.artswap").exists(),
              "its download data, deleted by the uninstall, is back as it was")
        held, sizes, _, _ = home_names(base, "PPSA99314")
        check(held == {"Eternal Darkness™"} and sizes, f"the original name is back in the database, every place ({held})")
        h.rows("HOMESTATE", "seen")
        apps = {r[1]: r for r in h.rows("APPS", "rescan")}
        check(apps["PPSA99131"][5].startswith("dds-bc7 3840x2160"), f"APPS: its background now ({apps['PPSA99131'][5]})")
        # Restore straight after the install: the install's copies go too
        report = h.rows("RESTORE", "PPSA99131", "all")
        check(not list(base.rglob("PPSA99131/**/pic0.dds")), f"RESTORE deletes the install's copies too ({report})")
        h.rows("APPLY", "PPSA99131", "pic", hero[2], "cover", "0", "0")
        shutil.copy(psp, base / "user/appmeta/PPSA99131/pic0.dds")  # as an install copies it
        h.rows("APPS", "rescan")
        h.rows("APPLY", "PPSA99131", "pic", lists["heroes"][-1][2], "fit", "0", "0")
        check(not list(base.rglob("PPSA99131/**/pic0.dds.old")), "changing an added background keeps no original")
        report = h.rows("RESTORE", "PPSA99131", "all")
        check(not list(base.rglob("PPSA99131/**/pic0.dds")), f"RESTORE deletes the added background, every copy ({report})")
        check("PPSA99131" not in (base / "data/artswap/added.txt").read_text(), "added.txt forgets it")
        pend = {r[1]: r[2] for r in h.rows("PENDING") if r[0] == "P"}
        check(pend.get("PPSA99131") == "install", f"the restore waits for an install too ({pend})")

        # music: songs of the person's, a clip of one as a title's snd0.at9
        music = base / "data/artswap/music"
        music.mkdir(parents=True, exist_ok=True)
        make_song(music / "Song.wav")
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(music / "Song.wav"), "-b:a", "192k",
                        str(music / "Song.mp3")], check=True)
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(music / "Song.wav"), str(music / "Song.flac")],
                       check=True)
        (music / "notes.txt").write_text("not music")
        rows = h.rows("SONGS")
        songs = [r[1] for r in rows if r[0] == "S"]
        check(songs == ["Song.flac", "Song.mp3", "Song.wav"], f"SONGS lists the music, A to Z ({songs})")
        maximum = float(next(r for r in rows if r[0] == "max")[1])
        check(86 < maximum < 88, f"SONGS: the longest clip ({maximum})")
        for name in songs:
            info = {r[0]: r for r in h.rows("SONG", name)}
            check(abs(float(info["song"][2]) - 40) < 0.2 and info["song"][3] == "44100" and info["song"][4] == "2"
                  and len(info["W"]) == 201, f"SONG {name}: 40 s, 44.1 kHz stereo, 200 levels ({info['song']})")
        raw = h.ask_raw("CLIP", "Song.flac", "5", "20")
        head, _, pcm = raw.partition(b"\n")
        fields = head.decode().split("\t")
        check(fields[0] == "clip" and fields[1] == "960000" and len(pcm) == 960000 * 4 and abs(float(fields[3]) + 28) < 0.5,
              f"CLIP: 20 s of 48 kHz stereo at -28 LUFS ({fields})")
        try:
            h.rows("SONG", "../apikey.txt")
            check(False, "SONG refuses a path")
        except HelperError:
            check(True, "SONG refuses a path")
        # a title with none: added to its folder, for an install to take in
        report = h.rows("MUSIC", "PPSA99314", "Song.mp3", "10", "30")
        snd = base / "data/homebrew/PPSA99314/sce_sys/snd0.at9"
        check(any(r[0] == "W" and r[1].endswith("sce_sys/snd0.at9") for r in report) and snd.exists(),
              f"MUSIC adds snd0.at9 to a folder game that had none ({report})")
        data = snd.read_bytes()
        check(data[:4] == b"RIFF" and data[8:12] == b"WAVE" and len(data) <= 2 * 1024 * 1024 and b"smpl" in data[:200],
              f"snd0.at9: RIFF/WAVE, looping, {len(data)} bytes")
        seconds, lufs = at9_loudness(snd)
        check(abs(seconds - 30.0) < 0.1 and lufs is not None and abs(lufs + 28) < 1.0,
              f"FFmpeg's ATRAC9 decoder plays it: {seconds:.2f} s at {lufs} LUFS")
        params = json.loads((base / "data/homebrew/PPSA99314/sce_sys/param.json").read_text())
        check(abs(float(params["pubtools"]["loudnessSnd0"]) + 28) < 0.5, f"param.json's loudnessSnd0 ({params['pubtools']})")
        apps = {r[1]: r for r in h.rows("APPS")}
        check(apps["PPSA99314"][16].startswith("at9 30s") and apps["PPSA99314"][17] == "1", f"APPS: its music, added ({apps['PPSA99314'][16:18]})")
        pend = {r[1]: r[2] for r in h.rows("PENDING") if r[0] == "P"}
        check(pend.get("PPSA99314") == "install", f"added music waits for an install ({pend})")
        # a title with its own: written over it, the original kept
        own = base / "user/appmeta/external/PPSA05678/snd0.at9"
        shutil.copy(snd, own)
        original_music = own.read_bytes()
        h.rows("APPS", "rescan")
        h.rows("MUSIC", "PPSA05678", "Song.wav", "0", "15")
        check(Path(str(own) + ".old").read_bytes() == original_music and own.read_bytes() != original_music,
              "MUSIC over a title's own music keeps the original")
        check(abs(at9_loudness(own)[0] - 15.0) < 0.1, "... and the new one is 15 s")
        pend = {r[1]: r[2] for r in h.rows("PENDING") if r[0] == "P"}
        check(pend.get("PPSA05678") == "art", f"new music over its own waits for a restart ({pend})")
        h.rows("RESTORE", "PPSA05678", "snd0")
        check(own.read_bytes() == original_music and not Path(str(own) + ".old").exists(), "RESTORE snd0 puts the original back")
        h.rows("RESTORE", "PPSA99314", "snd0")
        check(not snd.exists() and "snd0" not in (base / "data/artswap/added.txt").read_text(), "RESTORE deletes music Art Swap added")
        try:
            h.rows("MUSIC", "CUSA00001", "Song.wav", "0", "15")
            check(False, "no music is added to a game ShadowMountPlus cannot install again")
        except HelperError as e:
            check("installed from a folder" in str(e), f"no music is added to a package ({e})")
        # songs from a phone
        rows = dict(r[:2] for r in h.rows("PHONE", "4242", "music"))
        page = f"http://127.0.0.1:{PHONE_PORT}"
        time.sleep(0.5)
        html = urllib.request.urlopen(page + "/4242", timeout=5).read().decode()
        check("Send music to your PS5" in html and "value='4242'" in html, "the music page carries the code")

        def put(code, name, body):
            req = urllib.request.Request(f"{page}/upload?code={code}&name={urllib.parse.quote(name)}", data=body, method="PUT")
            try:
                with urllib.request.urlopen(req, timeout=30) as r:
                    return r.status
            except urllib.error.HTTPError as e:
                return e.code

        song_mp3 = (music / "Song.mp3").read_bytes()
        check(put("4242", "From my phone.mp3", song_mp3) == 200 and (music / "From my phone.mp3").read_bytes() == song_mp3,
              "a phone sends a song into the music folder")
        # as some phones' browsers send a file: in chunks, its size not announced first
        import http.client
        conn = http.client.HTTPConnection("127.0.0.1", PHONE_PORT, timeout=30)
        conn.putrequest("PUT", "/upload?code=4242&name=" + urllib.parse.quote("In chunks.mp3"))
        conn.putheader("Transfer-Encoding", "chunked")
        conn.endheaders()
        for at in range(0, len(song_mp3), 70000):
            piece = song_mp3[at:at + 70000]
            conn.send(b"%x\r\n" % len(piece) + piece + b"\r\n")
        conn.send(b"0\r\n\r\n")
        status = conn.getresponse().status
        conn.close()
        check(status == 200 and (music / "In chunks.mp3").read_bytes() == song_mp3, f"a song sent in chunks arrives whole ({status})")
        check(put("1111", "x.mp3", b"abc") == 403, "a wrong code sends nothing")
        check(put("4242", "../../apikey.txt", b"abc") == 400, "only music, by a plain name")
        check(not list(music.glob(".*partial")), "no partial file is left")
        h.ask("PHONE")
        time.sleep(1.0)

        pictures_tests(h, base, work)

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

        # a helper that ended while download data was aside: the next one puts it back as it starts
        os.rename(psp_download.parent, base / "user/download/PPSA99131.artswap")
        (base / "data/artswap/download-aside.txt").write_text("PPSA99131\n")
        proc = subprocess.Popen([str(helper_bin)], env=env, stdout=log, stderr=subprocess.STDOUT)
        check(wait_port(PORT), "the helper starts again")
        check(sha(psp_download) == psp_download_sha and not (base / "user/download/PPSA99131.artswap").exists() and
              (base / "data/artswap/download-aside.txt").read_text() == "",
              "download data left aside is moved back when the helper starts")
        Helper(port=PORT).ask("BYE")
        proc.wait(timeout=10)
    finally:
        if proc.poll() is None:
            proc.kill()
        log.close()
    layout_tests(work)
    print(f"\n{len(failures)} failed" if failures else "\nall passed")
    print(f"work folder: {work}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
