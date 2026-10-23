#!/usr/bin/env python3
# Art Swap - a stand-in for ShadowMountPlus's HTTP/JSON API (docs/api.md of 1.7), for tests.
#
#   fake_smp.py ROOT PORT        serves the made-up console under ROOT
#
# It knows the titles in GAMES. As on the console, games/uninstall takes the
# home screen's copies away (/user/app/<id>, /user/appmeta/<id>) and keeps the
# folder, and a scan installs the title again from its folder: the game list
# says "installed" at once, and the copies follow a second later, as
# ShadowMountPlus 1.7beta3 did on the console; the title's entry in the home
# screen's database loses its logo, as an install writes it afresh, and names
# the background and music the install found. The uninstall deletes the
# title's download data (/user/download/<id>), as the console's does. Switches, for tests (POST
# /fake {"busy": true, "eat": "relative/path"}): busy refuses uninstalls as
# while a game is open; eat deletes that file of the folder on uninstall, as a
# broken uninstall would.
#
# Copyright (C) 2026 Gary Riches
# SPDX-License-Identifier: GPL-3.0-or-later
import json
import shutil
import sqlite3
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

GAMES = {
    "PPSA99314": {"source_type": "folder", "path": "/data/homebrew/PPSA99314", "installed_pkg": False},
    "CUSA00001": {"source_type": "pkg", "path": "/user/app/CUSA00001/app.pkg", "installed_pkg": True},
    "PPSA26525": {"source_type": "image", "path": "/data/homebrew/PPSA26525_Heretic_Hexen.ffpfsc", "installed_pkg": False},
    "PPSA99131": {"source_type": "folder", "path": "/data/homebrew/PPSA99131", "installed_pkg": False},
}
CAPABILITIES = ["web_ui", "list_games", "game_info", "uninstall_game", "rescan"]


class Console:
    def __init__(self, root: Path):
        self.root = root
        self.lock = threading.Lock()
        self.installed = {title: True for title in GAMES}
        self.busy = False
        self.eat = ""
        self.calls = []

    def forget_logo(self, title):
        # the install writes the title's entry in the home screen's database afresh: no logo
        db_path = self.root / "system_data/priv/mms/app.db"
        if not db_path.exists():
            return
        db = sqlite3.connect(db_path)
        try:
            db.execute("UPDATE tbl_conceptmetadata SET logoImageInfo = NULL WHERE localConceptId = "
                       "(SELECT localConceptId FROM tbl_contentinfo WHERE titleId = ?)", (title,))
            db.commit()
        except sqlite3.OperationalError:
            pass
        db.close()

    def install(self, title):
        game = GAMES[title]
        folder = self.root / game["path"].lstrip("/")
        time.sleep(1.0)
        app = self.root / "user/app" / title
        meta = self.root / "user/appmeta" / title
        shutil.copytree(folder / "sce_sys", app / "sce_sys", dirs_exist_ok=True)
        (app / "mount.lnk").write_text(game["path"])
        meta.mkdir(parents=True, exist_ok=True)
        for f in (folder / "sce_sys").iterdir():
            if f.is_file() and not f.name.endswith(".old"):
                shutil.copy(f, meta / f.name)
        # the home screen's entry takes the name the install read, and the background and music it found
        db_path = self.root / "system_data/priv/mms/app.db"
        param = folder / "sce_sys/param.json"
        if db_path.exists() and param.exists():
            languages = json.loads(param.read_text())["localizedParameters"]
            name = languages[languages["defaultLanguage"]]["titleName"]
            stamp = int(time.time())
            pic0 = f"/user/app/{title}/sce_sys/pic0.dds?ts={stamp}" if (folder / "sce_sys/pic0.dds").exists() else ""
            snd0 = f"/user/app/{title}/sce_sys/snd0.at9?ts={stamp}" if (folder / "sce_sys/snd0.at9").exists() else ""
            db = sqlite3.connect(db_path)
            try:
                db.execute("UPDATE tbl_contentinfo SET titleName = ?, pic0Info = ?, snd0Info = ? WHERE titleId = ?",
                           (name, pic0, snd0, title))
                db.commit()
            except sqlite3.OperationalError:
                pass
            db.close()


def make_handler(console: Console):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def answer(self, code, body):
            data = json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            request = json.loads(self.rfile.read(length) or b"{}")
            route = self.path
            with console.lock:
                console.calls.append(route)
            if route == "/fake":
                console.busy = bool(request.get("busy", console.busy))
                console.eat = request.get("eat", console.eat)
                return self.answer(200, {"status": 0})
            if route == "/api/v1/version":
                return self.answer(200, {"status": 0, "api_version": 1, "shadowmount_version": "1.7fake",
                                         "capabilities": CAPABILITIES})
            if route == "/api/v1/games":
                games = [dict(GAMES[t], title_id=t, installed=console.installed[t], managed=not GAMES[t]["installed_pkg"],
                              source_available=True) for t in GAMES]
                return self.answer(200, {"status": 0, "count": len(games), "size_included": False, "games": games})
            if route == "/api/v1/games/uninstall":
                title = request.get("title_id", "")
                if title not in GAMES:
                    return self.answer(404, {"status": 2, "error": "No such file or directory"})
                if console.busy:
                    return self.answer(409, {"status": 16, "error": "Device busy", "error_reason": "game_active"})
                shutil.rmtree(console.root / "user/app" / title, ignore_errors=True)
                shutil.rmtree(console.root / "user/appmeta" / title, ignore_errors=True)
                shutil.rmtree(console.root / "user/download" / title, ignore_errors=True)
                console.forget_logo(title)
                if console.eat:
                    (console.root / GAMES[title]["path"].lstrip("/") / console.eat).unlink(missing_ok=True)
                console.installed[title] = False
                return self.answer(200, {"status": 0, "scan_queued": False, "scan_deferred": False,
                                         "scan_deferred_reason": ""})
            if route == "/api/v1/scan":
                for title, game in GAMES.items():
                    if not console.installed[title] and not game["installed_pkg"]:
                        console.installed[title] = True  # said at once; the copies come a second later
                        threading.Thread(target=console.install, args=(title,), daemon=True).start()
                return self.answer(200, {"status": 0, "scan_queued": True, "scan_deferred": False,
                                         "scan_deferred_reason": ""})
            return self.answer(404, {"status": 2, "error": "unknown route"})

    return Handler


def serve(root: Path, port: int) -> ThreadingHTTPServer:
    console = Console(root)
    server = ThreadingHTTPServer(("127.0.0.1", port), make_handler(console))
    server.console = console
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


if __name__ == "__main__":
    serve(Path(sys.argv[1]), int(sys.argv[2]))
    while True:
        time.sleep(3600)
