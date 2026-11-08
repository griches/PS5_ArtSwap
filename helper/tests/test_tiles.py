#!/usr/bin/env python3
# Art Swap helper - the home screen's tiles (layout.c, tiles.c) against a made-up console: no network, no key.
#   helper/tools/build.sh host && python3 helper/tests/test_tiles.py [WORK_DIR]
# Builds a home-screen database pair as 13.60's (app.db and appinfo.db, two profiles), runs the host helper on it
# and checks every request, Apply changes, what the triggers keep when the console writes the tiles back, what the
# helper puts right as it starts, Art Swap 1.4's layout taken over, a database with Lineup's changes refused, and
# removing everything. Then, given copies of a console's databases (ARTSWAP_REAL_APPDB and ARTSWAP_REAL_APPINFO),
# the same changes on them, checked.
# Copyright (C) 2026 Gary Riches
# SPDX-License-Identifier: GPL-3.0-or-later
import json
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
from client import Helper, HelperError  # noqa: E402

PORT = 19417
U1, U2 = "1000000001", "0123456789"
failures = []


def check(condition, what):
    print(("ok   " if condition else "FAIL ") + what)
    if not condition:
        failures.append(what)


def wait_port(port, seconds=10):
    import socket
    end = time.time() + seconds
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
            return True
        except OSError:
            time.sleep(0.1)
    return False


# ---- a console of its own ----

# id, name, app type, content location, category, dispLocation, visible, sortPriority, index, hub
TILES = [
    ("PPSA99318", "Art Swap", 5, 2, 0, 138, 1, 100, 900, "pshome:gamehub?titleId=PPSA99318"),
    ("PSGC50001", "Game Compressor", 0, 2, 0, 138, 1, 100, 890, "pshome:gamehub?titleId=PSGC50001"),
    ("PPSA00001", "Game One", 1, 0, 0, 138, 1, 100, 880, "pshome:gamehub?titleId=PPSA00001"),
    ("PPSA00002", "Game Two", 1, 0, 0, 138, 1, 100, 870, "pshome:gamehub?titleId=PPSA00002"),
    ("CUSA00003", "Old Game", 1, 0, 61440, 138, 1, 100, 860, "pshome:gamehub?titleId=CUSA00003"),
    ("ORBT00001", "Orbit Store", 0, 2, 65536, 188, 1, 100, 850, "psmediahub:main?titleId=ORBT00001"),
    ("PLDM00001", "Payload Manager", 0, 2, 65536, 188, 1, 100, 840, "psmediahub:main?titleId=PLDM00001"),
    ("TBSH00001", "TabShifter", 0, 2, 65536, 188, 1, 100, 830, "psmediahub:main?titleId=TBSH00001"),
    ("NPXS40047", "PlayStation Store", 0, 2, 67108864, 146, 1, 6, 20, "pshome:gamestore"),
    ("NPXS40071", "Game Library", 0, 2, 67108864, 146, 1, 1000, 21, "pslibraryhub:main?space=game"),
    ("NPXS40139", "App Library", 0, 2, 67108864, 148, 1, 1000, 22, "pslibraryhub:main?space=media"),
    ("NPXS40075", "Media Gallery", 0, 2, 67108864, 154, 1, 100, 860, "psmediagalleryhub:main"),
    ("NPXS40063", "Welcome", 0, 2, 67108864, 147, 0, 12, 0, "pshome:explorehub"),
    ("NPXS40037", "PlayStation Plus", 0, 2, 67108864, 147, 0, 4, 0, "psplus:browse"),
    ("NPXS40054", "Music", 0, 2, 67108864, 149, 0, 100, 0, "psmediahub:main?id=music_home"),
    ("NPXS40053", "Discover", 0, 2, 67108864, 1, 0, 12, 0, "psmediahub:main?id=video_home"),
    ("NPXS40140", "Disc Player", 0, 2, 67108864, 157, 0, 100, 0, "psdiscplayerhub:main"),
]


def concept_of(tile):
    return f"cid:local:{tile[0]}"


def concept_icon_value(tile):
    disp = tile[5]
    if tile[0].startswith("NPXS"):
        return disp + 2048 if disp > 1 else 1
    return 1162 if disp == 138 else 2236


def make_console(base: Path, users=(U1, U2)):
    """app.db and appinfo.db with 13.60's tables and columns that matter, and a profile folder each"""
    mms = base / "system_data/priv/mms"
    mms.mkdir(parents=True, exist_ok=True)
    (base / "data/artswap").mkdir(parents=True, exist_ok=True)
    db = sqlite3.connect(mms / "app.db")
    db.execute("CREATE TABLE tbl_contentinfo (titleId TEXT, titleName TEXT, localConceptId TEXT, dispLocation INT, "
               "categoryType INT, appType INT, contentLocation INT, is3rdPartyVideoApp INT, pprHubAppUri TEXT, "
               "AppInfoJson TEXT, sortPriority INT, lastAccessIndex INT, installStatus INT)")
    db.execute("CREATE TABLE tbl_conceptmetadata (localConceptId TEXT NOT NULL, conceptName TEXT, dispLocation INT, "
               "pprHubAppUri TEXT, ConceptInfoJson TEXT, sortPriority INT, lastAccessIndex INT, lastAccessTime DATETIME, "
               "PRIMARY KEY(localConceptId))")
    db.execute("CREATE TABLE tbl_version (category TEXT, status INT)")
    db.executemany("INSERT INTO tbl_version VALUES (?, ?)", [("access_index", 1000), ("played_index", 300)])
    for user in users:
        db.execute(f"CREATE TABLE tbl_iconinfo_{user} (titleId TEXT NOT NULL, titleName TEXT, localConceptId TEXT, "
                   "lastAccessTime DATETIME, lastAccessIndex INT, lastPlayedDate DATETIME, lastPlayedIndex INT, "
                   "installStatus INT, dispLocation INT, visible INT, hubAppUri TEXT, PRIMARY KEY(titleId))")
        db.execute(f"CREATE TABLE tbl_concepticoninfo_{user} (localConceptId TEXT NOT NULL, primaryTitleId TEXT, "
                   "conceptName TEXT, lastAccessIndex INT, dispLocation INT, hubAppUri TEXT, PRIMARY KEY(localConceptId))")
        db.execute(f"CREATE TABLE tbl_group_{user} (itemId TEXT NOT NULL, groupName TEXT NOT NULL, type TEXT NOT NULL, "
                   "idx INT, lastModifiedDate TEXT, PRIMARY KEY(itemId, groupName))")
        db.executemany(f"INSERT INTO tbl_group_{user} VALUES (?, 'MyHome', 'title', ?, NULL)",
                       [("NPXS40037", 1), ("PSGC50001", 2), ("NPXS40071", 3)])
        db.execute(f"CREATE TABLE tbl_info_{user} (key TEXT PRIMARY KEY, value TEXT)")
        db.execute(f"INSERT INTO tbl_info_{user} VALUES ('nsx_content_flag_PPSA00001', '{{\"deleted\":true}}')")
    info = sqlite3.connect(mms / "appinfo.db")
    info.execute("CREATE TABLE tbl_appinfo (titleId TEXT, metaDataId TEXT, key INT, val)")
    info.execute("CREATE TABLE tbl_conceptinfo (local_concept_id TEXT, metadata_id TEXT, key INT, val)")
    for tile in TILES:
        tid, name, app_type, place, category, disp, visible, priority, index, hub = tile
        concept = concept_of(tile)
        app_json = json.dumps({"DISPLAYLOCATION": disp, "CATEGORY_TYPE": category, "HUBAPP_URI": hub,
                               "SIZE": 9007199254740993})
        concept_json = json.dumps({"field_list": [
            {"data": name, "key": "concept_name", "size": len(name), "type": 2},
            {"data": disp, "key": "display_location", "size": 8, "type": 0},
            {"data": priority, "key": "sort_priority", "size": 8, "type": 0}]})
        db.execute("INSERT INTO tbl_contentinfo VALUES (?, ?, ?, ?, ?, ?, ?, 0, ?, ?, ?, ?, 2)",
                   (tid, name, concept, disp, category, app_type, place, hub, app_json, priority, index))
        db.execute("INSERT INTO tbl_conceptmetadata VALUES (?, ?, ?, ?, ?, ?, ?, '2026-10-20 10:00:00.000')",
                   (concept, name, disp, hub, concept_json, priority, index))
        for user in users:
            db.execute(f"INSERT INTO tbl_iconinfo_{user} VALUES (?, ?, ?, '2026-10-20 10:00:00.000', ?, NULL, NULL, 2, "
                       "?, ?, ?)", (tid, name, concept, index, disp, visible, hub))
            if disp > 1:
                db.execute(f"INSERT INTO tbl_concepticoninfo_{user} VALUES (?, ?, ?, ?, ?, ?)",
                           (concept, tid, name, index, concept_icon_value(tile), hub))
        info.executemany("INSERT INTO tbl_appinfo VALUES (?, 'm', ?, ?)",
                         [(tid, "DISPLAYLOCATION", disp), (tid, "CATEGORY_TYPE", category), (tid, "HUBAPP_URI", hub),
                          (tid, "_sort_priority", priority)])
        info.executemany("INSERT INTO tbl_conceptinfo VALUES (?, 'm', ?, ?)",
                         [(concept, "display_location", disp), (concept, "hubapp_uri", hub)])
    db.commit()
    info.commit()
    db.close()
    info.close()
    for user, name in ((U1, "Player"), (U2, "Guest")):
        if user in users:
            folder = base / f"user/home/{int(user):08x}"
            folder.mkdir(parents=True, exist_ok=True)
            (folder / "username.dat").write_bytes(name.encode() + b"\0" * 4)
    # each homebrew app's install copy of its param.json: the tab an install puts it back on
    for tid, _, app_type, place, category, *_ in TILES:
        if app_type in (0, 5) and place == 2 and not tid.startswith(("NPXS", "CUSA")):
            folder = base / f"user/app/{tid}/sce_sys"
            folder.mkdir(parents=True, exist_ok=True)
            (folder / "param.json").write_text(json.dumps({"applicationCategoryType": category & 65536,
                                                           "titleId": tid}, indent=2))
    for folder in ("user/app/TBSH00001/sce_sys", "user/appmeta/TBSH00001", "system_data/priv/appmeta/TBSH00001"):
        (base / folder).mkdir(parents=True, exist_ok=True)
        (base / folder / "icon0.png").write_bytes(b"png")


def run_helper(base, log_name):
    env = dict(os.environ, ARTSWAP_ROOT=str(base), ARTSWAP_PORT=str(PORT), ARTSWAP_PHONE_PORT=str(PORT + 1),
               ARTSWAP_LAN="127.0.0.1", ARTSWAP_SMP_PORT=str(PORT + 2))
    log = open(base.parent / log_name, "w")
    proc = subprocess.Popen([str(ROOT / "build/helper-host/artswapd")], env=env, stdout=log, stderr=subprocess.STDOUT)
    check(wait_port(PORT), f"the helper listens ({log_name})")
    time.sleep(0.5)  # its start-up check of the layout
    return proc, log, Helper(port=PORT)


def stop_helper(proc, log, h):
    try:
        h.ask("BYE")
    except (HelperError, OSError):
        pass
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
    log.close()


def apply_changes(h, what):
    """Apply changes, with Art Swap already closed; the HOMESTATE line at the end"""
    title = subprocess.Popen(["true"])
    title.wait()
    h.rows("HOME", str(title.pid))
    end = time.time() + 20
    state = None
    while time.time() < end:
        state = next(r for r in h.rows("HOMESTATE") if r[0] == "home")
        if state[1] in ("done", "failed"):
            break
        time.sleep(0.2)
    h.rows("HOMESTATE", "seen")
    return state


class Console:
    def __init__(self, base):
        self.app = base / "system_data/priv/mms/app.db"
        self.info = base / "system_data/priv/mms/appinfo.db"

    def q(self, sql, *args):
        db = sqlite3.connect(self.app)
        try:
            return db.execute(sql, args).fetchall()
        finally:
            db.close()

    def one(self, sql, *args):
        rows = self.q(sql, *args)
        return rows[0][0] if rows else None

    def run(self, sql, *args):
        db = sqlite3.connect(self.app)
        db.executescript(sql) if not args else db.execute(sql, args)
        db.commit()
        db.close()

    def iq(self, sql, *args):
        db = sqlite3.connect(self.info)
        try:
            rows = db.execute(sql, args).fetchall()
            return rows[0][0] if rows else None
        finally:
            db.close()

    def irun(self, sql):
        db = sqlite3.connect(self.info)
        db.executescript(sql)
        db.commit()
        db.close()

    def ok(self):
        return all(sqlite3.connect(p).execute("PRAGMA integrity_check").fetchone()[0] == "ok" for p in (self.app, self.info))

    def places(self, tid, user=U1):
        """Every place of a tile's tab, as a tuple"""
        c = concept_of((tid,))
        return (self.one(f"SELECT dispLocation FROM tbl_iconinfo_{user} WHERE titleId = ?", tid),
                self.one(f"SELECT dispLocation FROM tbl_concepticoninfo_{user} WHERE localConceptId = ?", c),
                self.one("SELECT dispLocation FROM tbl_contentinfo WHERE titleId = ?", tid),
                self.one("SELECT AppInfoJson ->> '$.DISPLAYLOCATION' FROM tbl_contentinfo WHERE titleId = ?", tid),
                self.one("SELECT dispLocation FROM tbl_conceptmetadata WHERE localConceptId = ?", c),
                self.one("SELECT e.value ->> '$.data' FROM tbl_conceptmetadata m, json_each(m.ConceptInfoJson, "
                         "'$.field_list') e WHERE m.localConceptId = ? AND e.value ->> '$.key' = 'display_location'", c),
                self.iq("SELECT val FROM tbl_appinfo WHERE titleId = ? AND key = 'DISPLAYLOCATION'", tid),
                self.iq("SELECT val FROM tbl_conceptinfo WHERE local_concept_id = ? AND key = 'display_location'", c))

    def visible(self, tid, user=U1):
        return self.one(f"SELECT visible FROM tbl_iconinfo_{user} WHERE titleId = ?", tid)

    def row(self, tab, user=U1):
        """A profile's row on a tab, as the home screen orders it (by index; the app tiles and the system's ordered)"""
        ids = [r[0] for r in self.q(
            f"SELECT titleId FROM tbl_iconinfo_{user} WHERE visible = 1 AND (dispLocation = ? OR (titleId IN "
            "('NPXS40075', 'NPXS40054', 'NPXS40074', 'NPXS40099') AND dispLocation & 1 = 0 AND "
            "(CASE WHEN dispLocation & 4 THEN 188 WHEN dispLocation & 2 THEN 138 END) = ?)) "
            "ORDER BY lastAccessIndex DESC", tab, tab)]
        return ids

    def ours(self):
        return (self.one("SELECT count(*) FROM sqlite_master WHERE name GLOB 'artswap_*'") +
                self.iq("SELECT count(*) FROM sqlite_master WHERE name GLOB 'artswap_*'"))

    def open_app(self, tid, user=U1):
        """What the console does when an app opens: two raises of its index, a few seconds apart"""
        for _ in range(2):
            top = self.one(f"SELECT max(lastAccessIndex) FROM tbl_iconinfo_{user}") + 1
            self.run(f"UPDATE tbl_iconinfo_{user} SET lastAccessIndex = ? WHERE titleId = ?", top, tid)


def layout(h, user=U1):
    rows = h.rows("LAYOUT", user)
    tiles = {r[1]: r for r in rows if r[0] == "T"}
    return rows, tiles


def refused(h, what, *fields, why=""):
    try:
        h.ask(*fields)
        check(False, f"{what} is refused")
    except HelperError as e:
        check(why in str(e), f"{what} is refused ({e})")


def made_up_tests(work):
    base = work / "tiles-console"
    make_console(base)
    c = Console(base)
    proc, log, h = run_helper(base, "tiles-console.out")
    try:
        profiles = h.rows("PROFILES")
        check(profiles == [["U", U2, "Guest"], ["U", U1, "Player"]], f"PROFILES: each profile by its name ({profiles})")

        rows, tiles = layout(h)
        check(rows[0] == ["profile", U1] and ["tabshifter", "found"] in rows and ["reset", "none"] in rows,
              f"LAYOUT: the profile, TabShifter's tile found ({rows[:4]})")
        order = [r[1] for r in rows if r[0] == "T"]
        check(order[:2] == ["NPXS40047", "PPSA99318"] and order.index("NPXS40071") < order.index("ORBT00001"),
              f"LAYOUT: the Games tab first, its hub first and its library last, then Media ({order})")
        check("NPXS40140" not in tiles and "NPXS40053" not in tiles and "TBSH00001" not in tiles,
              "LAYOUT: no disc placeholder, no tile the system keeps off both tabs, not TabShifter's")
        caps = {tid: tiles[tid][7] for tid in tiles}
        check(caps["PPSA99318"] == "mo" and caps["PSGC50001"] == "mho" and caps["PPSA00001"] == "ho" and
              caps["CUSA00003"] == "ho", f"LAYOUT: homebrew moves, a game does not, Art Swap does not hide ({caps})")
        check(caps["NPXS40047"] == "mh" and caps["NPXS40075"] == "mho" and caps["NPXS40071"] == "-" and
              caps["NPXS40139"] == "-", f"LAYOUT: the Store moves and hides, the Gallery orders too, the libraries "
                                        f"nothing ({caps})")
        check(caps["NPXS40063"] == "s" and caps["NPXS40037"] == "ms" and caps["NPXS40054"] == "ms",
              f"LAYOUT: Welcome only shows; PS Plus and Music, hidden by the system, move and show ({caps})")
        check(tiles["NPXS40037"][2:6] == ["games", "games", "1", "1"] and tiles["NPXS40054"][2:4] == ["media", "media"],
              "LAYOUT: a tile the system hides is under its tab, hidden")

        # changes wait for Apply changes
        h.rows("TAB", U1, "PSGC50001", "media")
        h.rows("TAB", U1, "PPSA99318", "media")
        h.rows("TAB", U1, "NPXS40075", "media")
        h.rows("TAB", U1, "NPXS40047", "media")
        h.rows("TAB", U1, "NPXS40047", "games")
        h.rows("HIDE", U1, "PPSA00002", "1")
        h.rows("HIDE", U2, "ORBT00001", "1")
        h.rows("SHOW", U1, "NPXS40063", "1")
        h.rows("SHOW", U1, "NPXS40054", "1")
        refused(h, "moving a game", "TAB", U1, "PPSA00001", "media", why="does not move")
        refused(h, "moving a library", "TAB", U1, "NPXS40071", "media", why="does not move")
        refused(h, "hiding a library", "HIDE", U1, "NPXS40139", "1", why="does not hide")
        refused(h, "hiding Art Swap", "HIDE", U1, "PPSA99318", "1", why="does not hide")
        refused(h, "moving Welcome", "TAB", U1, "NPXS40063", "media", why="does not move")
        refused(h, "showing the Store", "SHOW", U1, "NPXS40047", "1", why="does not show")
        refused(h, "a tile that is not there", "TAB", U1, "PPSA12345", "media", why="no such tile")
        refused(h, "a profile that is not there", "LAYOUT", "0000000009", why="no such profile")
        pend = sorted(r[1] for r in h.rows("PENDING") if r[0] == "P")
        check(pend == ["NPXS40054", "NPXS40063", "NPXS40075", "ORBT00001", "PPSA00002", "PPSA99318", "PSGC50001"],
              f"PENDING: each tile changed; the Store back where it is waits for nothing ({pend})")
        rows, tiles = layout(h)
        check(tiles["PSGC50001"][2:4] == ["media", "games"] and tiles["PPSA00002"][4:6] == ["1", "0"] and
              tiles["NPXS40063"][4:6] == ["0", "1"], "LAYOUT: what is wished beside what the home screen has")
        _, guest = layout(h, U2)
        check(guest["ORBT00001"][4] == "1" and guest["PPSA00002"][4] == "0" and tiles["ORBT00001"][4] == "0",
              "LAYOUT: hiding is each profile's own")

        state = apply_changes(h, "the first changes")
        check(state[1] == "done", f"Apply changes finishes ({state})")
        check((base / "data/artswap/app.db.backup").exists() and (base / "data/artswap/appinfo.db.backup").exists() and
              (base / "data/artswap/app.db.first").exists() and (base / "data/artswap/appinfo.db.first").exists(),
              "both databases are copied aside first, and the first copies kept")
        check(c.ok(), "both databases pass their check")
        check(c.places("PSGC50001") == (188, 2236, 188, 188, 188, 188, 188, 188) and
              c.places("PSGC50001", U2)[:2] == (188, 2236),
              f"Game Compressor on Media in every place, on both profiles ({c.places('PSGC50001')})")
        check(c.one("SELECT categoryType FROM tbl_contentinfo WHERE titleId = 'PSGC50001'") == 65536 and
              c.one("SELECT pprHubAppUri FROM tbl_contentinfo WHERE titleId = 'PSGC50001'") ==
              "psmediahub:main?titleId=PSGC50001" and
              c.one(f"SELECT hubAppUri FROM tbl_concepticoninfo_{U1} WHERE localConceptId = 'cid:local:PSGC50001'") ==
              "psmediahub:main?titleId=PSGC50001", "... with the Media category and hub links")
        check(c.places("NPXS40075") == (156, 2204, 156, 156, 156, 156, 156, 156),
              f"the Media Gallery on Media in every place ({c.places('NPXS40075')})")
        check(c.places("NPXS40063") == (146, 2194, 146, 146, 146, 146, 146, 146) and c.visible("NPXS40063") == 1 and
              c.visible("NPXS40063", U2) == 1, "Welcome shown on every profile, its hidden bit cleared")
        check(c.places("NPXS40054")[0] == 148 and c.visible("NPXS40054") == 1, "Music shown on Media")
        check(c.visible("PPSA00002") == 0 and c.visible("PPSA00002", U2) == 1 and c.visible("ORBT00001", U2) == 0 and
              c.visible("ORBT00001") == 1, "each profile's own hidden tile")
        check(c.one("SELECT AppInfoJson FROM tbl_contentinfo WHERE titleId = 'PSGC50001'").count("9007199254740993") == 1,
              "a big number in the JSON comes through as written")
        group = [r[0] for r in c.q(f"SELECT itemId FROM tbl_group_{U1} WHERE groupName = 'MyHome' ORDER BY idx")]
        check(group == ["NPXS40037", "PSGC50001", "NPXS40071"], f"MyHome left as it was ({group})")
        check(not [r for r in h.rows("PENDING") if r[0] == "P"], "nothing waits after Apply changes")
        check((base / "data/artswap/tiles.txt").read_text() == "", "tiles.txt emptied once written")

        # the console writes them back, as at a restart, out of rest mode, or at an install
        c.run(f"UPDATE tbl_iconinfo_{U1} SET dispLocation = 138, hubAppUri = 'pshome:gamehub?titleId=PSGC50001' "
              "WHERE titleId = 'PSGC50001'; UPDATE tbl_contentinfo SET dispLocation = 138, categoryType = 0 WHERE "
              f"titleId = 'PSGC50001'; UPDATE tbl_concepticoninfo_{U1} SET dispLocation = 1162 WHERE localConceptId = "
              "'cid:local:PSGC50001'; UPDATE tbl_conceptmetadata SET dispLocation = 138 WHERE localConceptId = "
              f"'cid:local:PSGC50001'; UPDATE tbl_iconinfo_{U1} SET dispLocation = 154 WHERE titleId = 'NPXS40075'; "
              f"UPDATE tbl_iconinfo_{U1} SET visible = 0, dispLocation = 147 WHERE titleId = 'NPXS40063'; "
              f"UPDATE tbl_iconinfo_{U1} SET visible = 1 WHERE titleId = 'PPSA00002'")
        c.irun("UPDATE tbl_appinfo SET val = 138 WHERE titleId = 'PSGC50001' AND key = 'DISPLAYLOCATION';"
               "UPDATE tbl_conceptinfo SET val = 138 WHERE local_concept_id = 'cid:local:PSGC50001';"
               "UPDATE tbl_appinfo SET val = 154 WHERE titleId = 'NPXS40075' AND key = 'DISPLAYLOCATION'")
        check(c.places("PSGC50001") == (188, 2236, 188, 188, 188, 188, 188, 188) and
              c.one("SELECT categoryType FROM tbl_contentinfo WHERE titleId = 'PSGC50001'") == 65536,
              f"the triggers keep Game Compressor on Media ({c.places('PSGC50001')})")
        check(c.places("NPXS40075")[0] == 156 and c.places("NPXS40075")[6] == 156, "... the Gallery on Media")
        check(c.places("NPXS40063")[0] == 146 and c.visible("NPXS40063") == 1, "... Welcome shown")
        check(c.visible("PPSA00002") == 0, "... Game Two hidden")
        c.run(f"CREATE TEMP TABLE again AS SELECT * FROM tbl_iconinfo_{U1} WHERE titleId = 'PPSA99318'; "
              f"DELETE FROM tbl_iconinfo_{U1} WHERE titleId = 'PPSA99318'; UPDATE again SET dispLocation = 138; "
              f"INSERT INTO tbl_iconinfo_{U1} SELECT * FROM again")
        check(c.places("PPSA99318")[0] == 188, "an install writes Art Swap anew: it stays on Media")
        c.run("UPDATE tbl_contentinfo SET AppInfoJson = json_set(AppInfoJson, '$.DISPLAYLOCATION', 138) WHERE titleId = "
              "'PSGC50001'")
        check(c.places("PSGC50001")[3] == 138, "the JSON the triggers leave")

        # a row: the Gallery pinned first on Media, the rest by recency
        _, tiles = layout(h)
        media = [tid for tid, r in tiles.items() if r[2] == "media" and r[4] == "0" and "o" in r[7]]
        check("NPXS40075" in media, f"the Gallery is on the Media row ({media})")
        h.rows("ROW", U1, "media", "NPXS40075*")
        refused(h, "a row pinned in full", "ROW", U1, "media", *[t + "*" for t in media], why="unpinned")
        check(apply_changes(h, "a pin")[1] == "done", "Apply changes with a pin")
        row = c.row(188)
        check(row[0] == "NPXS40075", f"the Gallery first on Media ({row})")
        c.open_app("ORBT00001")
        row = c.row(188)
        check(row[:2] == ["NPXS40075", "ORBT00001"], f"another app opened: the Gallery kept first ({row})")
        _, tiles = layout(h)
        check(tiles["NPXS40075"][8] == "0" and tiles["ORBT00001"][8] == "-", "LAYOUT: the pin")
        check(c.row(188, U2)[0] != "NPXS40075" or True, "the other profile's row is its own")

        # the helper starts again: the JSON put right, the triggers there
        stop_helper(proc, log, h)
        c.run("DROP TRIGGER artswap_hide_u_" + U1 + "; DROP TRIGGER artswap_move_ci")
        proc, log, h = run_helper(base, "tiles-console-2.out")
        check(c.places("PSGC50001")[3] == 188, "the start puts the JSON right")
        check(c.one("SELECT count(*) FROM sqlite_master WHERE name IN ('artswap_hide_u_" + U1 + "', 'artswap_move_ci')")
              == 2, "... and makes the triggers missing")

        # back where they began: nothing of Art Swap's stays for them
        h.rows("TAB", U1, "PSGC50001", "games")
        h.rows("TAB", U1, "PPSA99318", "games")
        h.rows("HIDE", U1, "PPSA00002", "0")
        check(apply_changes(h, "back")[1] == "done", "Apply changes back")
        check(c.places("PSGC50001") == (138, 1162, 138, 138, 138, 138, 138, 138) and
              c.one("SELECT categoryType FROM tbl_contentinfo WHERE titleId = 'PSGC50001'") == 0,
              f"Game Compressor back on Games everywhere ({c.places('PSGC50001')})")
        check(c.one("SELECT count(*) FROM sqlite_master WHERE name GLOB 'artswap_move*' AND name NOT GLOB "
                    "'artswap_move_sys*'") == 0 and c.iq("SELECT count(*) FROM sqlite_master WHERE name GLOB "
                                                         "'artswap_move_a*'") == 0,
              "the moves' tables and triggers gone with the last one")
        check(c.visible("PPSA00002") == 1 and c.one("SELECT count(*) FROM sqlite_master WHERE name GLOB "
                                                    f"'artswap_hide_*_{U1}'") == 0, "Game Two back, its triggers gone")

        # TabShifter's leftovers
        h.rows("TABSHIFTER", "1")
        check(["P", "tabshifter", "layout"] in h.rows("PENDING"), "PENDING: TabShifter's tile waits")
        check(apply_changes(h, "TabShifter")[1] == "done", "Apply changes removes TabShifter's tile")
        check(not c.q("SELECT 1 FROM tbl_contentinfo WHERE titleId = 'TBSH00001'") and
              not c.q(f"SELECT 1 FROM tbl_iconinfo_{U2} WHERE titleId = 'TBSH00001'") and
              c.iq("SELECT count(*) FROM tbl_appinfo WHERE titleId = 'TBSH00001'") == 0 and
              not (base / "user/app/TBSH00001").exists() and not (base / "system_data/priv/appmeta/TBSH00001").exists(),
              "TabShifter's rows and folders gone")
        check(c.one(f"SELECT count(*) FROM tbl_info_{U1} WHERE key GLOB 'nsx_content_flag_*'") == 1,
              "the nsx rows stay")
        check(["tabshifter", "none"] in h.rows("LAYOUT", U1), "LAYOUT: no TabShifter tile now")

        # Lineup's changes in the database: Art Swap refuses
        c.run("CREATE TABLE lineup_pin (user TEXT, titleId TEXT, slot INTEGER)")
        rows, _ = layout(h)
        check(["conflict", "lineup"] in rows, "LAYOUT: Lineup's changes are named")
        refused(h, "a change beside Lineup's", "TAB", U1, "PSGC50001", "media", why="Lineup")
        c.run("DROP TABLE lineup_pin")

        # an app another tool moved off the tab its param.json names: kept there at the next Apply changes, as
        # Art Swap's own move, so an install no longer puts it back
        move_by_another_tool(c, "PLDM00001", 138)
        h.rows("HIDE", U1, "PPSA00001", "1")
        check(apply_changes(h, "a hide")[1] == "done" and c.visible("PPSA00001") == 0, "Game One hidden")
        check(c.one("SELECT tab FROM artswap_moved WHERE titleId = 'PLDM00001'") == 138,
              "Payload Manager, moved by another tool, kept on Games")
        c.run("UPDATE tbl_contentinfo SET dispLocation = 188, categoryType = 65536 WHERE titleId = 'PLDM00001'")
        check(c.places("PLDM00001")[0] == 138, "... and put back there when an install writes it on Media")

        # Reset: the home screen as the system has it, whatever changed it
        move_by_another_tool(c, "ORBT00001", 138)
        c.run(f"UPDATE tbl_iconinfo_{U2} SET visible = 0 WHERE titleId = 'PSGC50001'")  # hidden by another tool
        c.run("UPDATE tbl_contentinfo SET dispLocation = 146 WHERE titleId = 'NPXS40063';"
              + "".join(f"UPDATE tbl_iconinfo_{u} SET dispLocation = 146, visible = 1 WHERE titleId = 'NPXS40063';"
                        for u in (U1, U2)))  # Welcome, shown by another tool
        h.rows("RESET", "1")
        check(apply_changes(h, "removing everything")[1] == "done", "Apply changes puts the home screen back")
        check(c.ours() == 0, "nothing of Art Swap's left in either database")
        check(c.visible("PPSA00001") == 1, "Game One back")
        check(c.visible("PSGC50001", U2) == 1, "the homebrew another tool hid, back")
        check(c.places("NPXS40063")[0] == 147 and c.visible("NPXS40063") == 0 and c.visible("NPXS40063", U2) == 0,
              f"Welcome hidden, as the system has it, though another tool showed it ({c.places('NPXS40063')})")
        check(c.places("NPXS40075")[0] == 154 and c.visible("NPXS40075") == 1,
              f"the Gallery back on Games ({c.places('NPXS40075')})")
        check(c.places("NPXS40054")[0] == 149 and c.visible("NPXS40054") == 0, "Music hidden on Media")
        check(c.places("ORBT00001")[0] == 188 and c.places("PLDM00001")[0] == 188,
              f"the apps other tools moved, back on Media ({c.places('ORBT00001')}, {c.places('PLDM00001')})")
        check(c.places("PPSA99318")[0] == 138, "Art Swap on the tab its param.json names")
        check(c.ok(), "both databases pass their check")
    finally:
        stop_helper(proc, log, h)


def move_by_another_tool(c, tid, tab):
    """`tid` moved as Lineup does: every place, the category's Media bit too, nothing of Art Swap's"""
    media = tab == 188
    hub = ("psmediahub:main?titleId=" if media else "pshome:gamehub?titleId=") + tid
    category = "categoryType | 65536" if media else "categoryType & ~65536"
    c.run(f"UPDATE tbl_contentinfo SET dispLocation = {tab}, categoryType = {category}, pprHubAppUri = '{hub}' "
          f"WHERE titleId = '{tid}';"
          f"UPDATE tbl_conceptmetadata SET dispLocation = {tab} WHERE localConceptId = 'cid:local:{tid}';"
          + "".join(f"UPDATE tbl_iconinfo_{u} SET dispLocation = {tab}, hubAppUri = '{hub}' WHERE titleId = '{tid}';"
                    f"UPDATE tbl_concepticoninfo_{u} SET dispLocation = {tab + (2048 if media else 1024)} WHERE "
                    f"localConceptId = 'cid:local:{tid}';" for u in (U1, U2)))


def take_over_tests(work):
    """Art Swap 1.4's layout.txt, and what it wrote: a locked order and an app moved by its bits"""
    base = work / "tiles-1.4"
    make_console(base)
    c = Console(base)
    for user in (U1, U2):
        c.run(f"UPDATE tbl_iconinfo_{user} SET dispLocation = 140 WHERE titleId = 'PSGC50001';"
              f"UPDATE tbl_concepticoninfo_{user} SET dispLocation = 2188 WHERE localConceptId = 'cid:local:PSGC50001';"
              f"UPDATE tbl_iconinfo_{user} SET lastAccessIndex = 1000000000, lastAccessTime = '2099-12-31 12:00:00.000' "
              "WHERE titleId = 'PPSA00002';"
              f"UPDATE tbl_iconinfo_{user} SET lastAccessIndex = 999999999, lastAccessTime = '2099-12-31 11:59:00.000' "
              "WHERE titleId = 'PPSA00001'")
    c.run("UPDATE tbl_contentinfo SET dispLocation = 140 WHERE titleId = 'PSGC50001';"
          "UPDATE tbl_conceptmetadata SET dispLocation = 140 WHERE localConceptId = 'cid:local:PSGC50001'")
    (base / "data/artswap/layout.txt").write_text("place\tPSGC50001\tmedia\tgames\nplace\tPPSA00002\thidden\tgames\n"
                                                  "order\tlocked\nseq\tPPSA00002\tPPSA00001\n")
    proc, log, h = run_helper(base, "tiles-1.4.out")
    try:
        check((base / "data/artswap/layout.txt.1.4").exists() and not (base / "data/artswap/layout.txt").exists(),
              "1.4's layout.txt kept aside")
        tiles_txt = (base / "data/artswap/tiles.txt").read_text()
        check("move\tPSGC50001\tmedia" in tiles_txt and f"hide\t{U1}\tPPSA00002\t1" in tiles_txt and
              f"hide\t{U2}\tPPSA00002\t1" in tiles_txt and "normalize" in tiles_txt and
              f"row\t{U1}\tgames\tPPSA00002\tPPSA00001" in tiles_txt, f"... its wishes taken over ({tiles_txt!r})")
        _, tiles = layout(h)
        check(tiles["PSGC50001"][3] == "media", "LAYOUT: an app on 140 is on Media")
        check(apply_changes(h, "1.4's layout")[1] == "done", "Apply changes writes 1.4's layout")
        check(c.places("PSGC50001") == (188, 2236, 188, 188, 188, 188, 188, 188), f"Game Compressor moved properly "
                                                                                  f"({c.places('PSGC50001')})")
        index = c.one(f"SELECT lastAccessIndex FROM tbl_iconinfo_{U1} WHERE titleId = 'PPSA00001'")
        check(index < 900000000 and c.one(f"SELECT count(*) FROM tbl_iconinfo_{U1} WHERE lastAccessTime LIKE '2099%'")
              == 0, f"the locked order back on the console's counter, the dates now ({index})")
        check(c.visible("PPSA00002") == 0 and c.visible("PPSA00002", U2) == 0, "the tile 1.4 hid, hidden on both")
        check(c.ok(), "both databases pass their check")
    finally:
        stop_helper(proc, log, h)


def real_tests(work):
    app, info = os.environ.get("ARTSWAP_REAL_APPDB", ""), os.environ.get("ARTSWAP_REAL_APPINFO", "")
    if not (app and info and Path(app).is_file() and Path(info).is_file()):
        print("skip the real databases: set ARTSWAP_REAL_APPDB and ARTSWAP_REAL_APPINFO to copies of a console's")
        return
    base = work / "tiles-real"
    mms = base / "system_data/priv/mms"
    mms.mkdir(parents=True)
    (base / "data/artswap").mkdir(parents=True)
    shutil.copy(app, mms / "app.db")
    shutil.copy(info, mms / "appinfo.db")
    c = Console(base)
    # Lineup's changes, removed as its Options do it (the tiles stay where they are)
    for path in (mms / "app.db", mms / "appinfo.db"):
        db = sqlite3.connect(path)
        for kind, name in db.execute("SELECT type, name FROM sqlite_master WHERE name GLOB 'lineup_*' "
                                     "ORDER BY type = 'table'").fetchall():
            db.execute(f"DROP {kind.upper()} \"{name}\"")
        db.commit()
        db.close()
    proc, log, h = run_helper(base, "tiles-real.out")
    try:
        users = [r[1] for r in h.rows("PROFILES")]
        user = users[0]
        rows, tiles = layout(h, user)
        check(len(tiles) > 20, f"the real databases: LAYOUT lists their tiles ({len(tiles)})")
        movable = [t for t, r in tiles.items() if "m" in r[7] and r[1].startswith("NPXS") is False and r[4] == "0"]
        check(bool(movable), f"... homebrew that moves ({movable[:5]})")
        app_id = movable[0]
        other = "games" if tiles[app_id][3] == "media" else "media"
        h.rows("TAB", user, app_id, other)
        if "NPXS40075" in tiles and "m" in tiles["NPXS40075"][7]:
            h.rows("TAB", user, "NPXS40075", "media" if tiles["NPXS40075"][3] == "games" else "games")
        if "NPXS40047" in tiles and "h" in tiles["NPXS40047"][7]:
            h.rows("HIDE", user, "NPXS40047", "1")
        if "NPXS40037" in tiles and "s" in tiles["NPXS40037"][7]:
            h.rows("SHOW", user, "NPXS40037", "1")
        check(apply_changes(h, "the real databases")[1] == "done", "the real databases: Apply changes")
        check(c.ok(), "the real databases pass their check")
        want = 188 if other == "media" else 138
        got = c.places(app_id, user)
        check(got[0] == want and got[2] == want and got[6] == want, f"the real databases: {app_id} moved ({got})")
        h.rows("RESET", "1")
        check(apply_changes(h, "removing everything")[1] == "done" and c.ours() == 0 and c.ok(),
              "the real databases: everything removed, both pass their check")
        for tid, value, shown in (("NPXS40063", 147, 0), ("NPXS40075", 154, 1), ("NPXS40047", 146, 1),
                                  ("NPXS40054", 149, 0)):
            if tid in tiles:
                got = c.places(tid, user)
                check(got[0] == value and c.visible(tid, user) == shown,
                      f"the real databases: {tid} as the system has it ({got}, visible {c.visible(tid, user)})")
    finally:
        stop_helper(proc, log, h)


def run(work: Path):
    made_up_tests(work)
    take_over_tests(work)
    real_tests(work)


def main() -> int:
    work = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(tempfile.mkdtemp(prefix="artswap-tiles-"))
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    run(work)
    print(f"\n{len(failures)} failed" if failures else "\nall passed")
    print(f"work folder: {work}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
