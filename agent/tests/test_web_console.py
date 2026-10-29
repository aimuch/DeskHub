"""The files of the web console (agent/src/deskhub_agent/web), checked before a browser opens them.

Ways this can fail, and what stops each (the browser flows are in scripts/e2e/console.e2e.mjs):
W1  A page or script that the Content-Security-Policy blocks: inline script, inline style, an on-click attribute, an
    external stylesheet or font, an @import, a url(http...).
W2  A script that does not parse, or one that uses a way to write untrusted text as markup (innerHTML, insertAdjacentHTML,
    eval, new Function, document.write).
W3  A secret in the browser's storage: the only thing allowed there is the list of appearance choices, from one file, under
    one key, and only values from a fixed list.
W4  A file the server does not send whole (the 16 KiB cap is for JSON, not for static files), or one sent with the wrong type.
W5  A nav entry without a page, a page without a nav entry, or a page that is not in the design (Web-* boards).
W6  A management route that answers without a session. Only /auth, /login and /session (POST) are open.
W7  A request that changes something and does not send the CSRF header (login and session start are the only exceptions).
W8  A word the product removed: the "experimental" marker on the screen (rule R1).
"""
from html.parser import HTMLParser
from pathlib import Path
import re
import shutil
import subprocess

from fastapi.testclient import TestClient

from deskhub_agent.app import create_app

WEB = Path(__file__).resolve().parents[1] / "src/deskhub_agent/web"
PAGES = ["overview", "devices", "actions", "commands", "layout", "power", "firmware", "access", "appearance"]   # Web-* boards, in the order of the nav


class Assets(HTMLParser):
    def __init__(self):
        super().__init__()
        self.paths, self.handlers, self.inline_script, self.inline_style, self.external = [], [], False, False, []

    def handle_starttag(self, tag, attrs):
        attributes = dict(attrs)
        if tag == "script" and not attributes.get("src"):
            self.inline_script = True
        if tag == "style" or "style" in attributes:
            self.inline_style = True
        self.handlers.extend(key for key in attributes if key.startswith("on"))
        if tag in {"script", "link"}:
            path = attributes.get("src") or attributes.get("href")
            if path and path.startswith("/manage/"):
                self.paths.append(path)
            elif path and re.match(r"^(https?:)?//", path):
                self.external.append(path)


def test_w1_w4_files_load_whole_under_the_real_headers(settings):
    with TestClient(create_app(settings)) as client:
        response = client.get("/manage/")
        assert response.status_code == 200 and "text/html" in response.headers["content-type"]
        assert response.headers["x-content-type-options"] == "nosniff" and response.headers["x-frame-options"] == "DENY"
        policy = response.headers["content-security-policy"]
        assert "script-src 'self'" in policy and "style-src 'self'" in policy and "unsafe-inline" not in policy
        page = Assets()
        page.feed(response.text)
        assert not page.handlers and not page.inline_script and not page.inline_style and not page.external
        assert len(page.paths) >= 2                                   # at least the stylesheet and the first script
        for path in page.paths:
            asset = client.get(path)
            assert asset.status_code == 200 and asset.content, path
            assert "text/html" not in asset.headers.get("content-type", ""), path
        for file in WEB.rglob("*"):
            if file.is_file() and file.suffix in {".js", ".css", ".svg", ".html"}:
                served = client.get("/manage/" + file.relative_to(WEB).as_posix())
                assert served.status_code == 200 and served.content == file.read_bytes(), file
        sheet = (WEB / "styles.css").read_text()
        assert "@import" not in sheet and not re.search(r"url\(\s*['\"]?(https?:)?//", sheet)


def test_w2_w3_scripts_parse_and_never_write_markup_or_secrets():
    node = shutil.which("node")
    modules = sorted(WEB.rglob("*.js"))
    assert modules
    for script in modules:
        source = script.read_text()
        if node:
            subprocess.run([node, "--input-type=module", "--check"], input=script.read_bytes(), check=True, capture_output=True, timeout=20)
        for unsafe in (".innerHTML", "outerHTML", "insertAdjacentHTML", "eval(", "new Function", "document.write", "sessionStorage", "indexedDB", ".style.", "setAttribute('style'"):
            assert unsafe not in source, (script.name, unsafe)
        assert not re.search(r"""['"]https?://""", source.replace("'http://www.w3.org/2000/svg'", "")), script.name   # the SVG namespace is a name, not an address
        if "localStorage" in source:
            assert script.name == "prefs.js", f"{script.name} writes to the browser storage"
    prefs = (WEB / "lib/prefs.js").read_text()
    assert prefs.count("localStorage") >= 2 and prefs.count("'deskhub.appearance'") == 1
    for secret in ("password", "token", "csrf", "cookie"):
        assert secret not in prefs.lower(), secret


def test_w5_w8_the_nav_and_the_design_agree_and_no_experimental_word():
    app = (WEB / "app.js").read_text()
    for page in PAGES:
        assert (WEB / "pages" / f"{page}.js").is_file(), page
        assert re.search(rf"\b{page}:\s*\{{", app), page
    assert len(re.findall(r"^\s{2}\w+: \{ title:", app, flags=re.M)) == len(PAGES)
    for file in WEB.rglob("*"):
        if file.is_file() and file.suffix in {".js", ".html", ".css"}:
            assert "实验功能" not in file.read_text(), file


def test_w6_nothing_answers_without_a_session(settings):
    with TestClient(create_app(settings)) as client:
        for path in ("overview", "host", "displays", "commands", "devices", "devices/x", "firmware", "pairing-grants"):
            assert client.get(f"/api/v1/admin/{path}").status_code == 401, path
        assert client.get("/api/v1/admin/auth").status_code == 200
        assert client.get("/api/v1/admin/session").status_code == 401
        assert client.put("/api/v1/admin/password", json={"password": "q7Z!mK2#pL9v"}).status_code == 401


def test_w7_every_write_sends_the_csrf_header():
    api = (WEB / "lib/api.js").read_text()
    assert re.search(r"headers\.set\('X-CSRF-Token', getCsrf\(\)\)", api)
    # A write may skip the header only on the two sign-in routes (the session does not exist yet).
    skip = re.search(r"^.*method !== 'GET'.*$", api, flags=re.M)
    assert skip and "'/session'" in skip.group(0) and "'/login'" in skip.group(0) and "'/password'" not in skip.group(0)
    assert "X-CSRF-Token" in api.split("function upload")[1]
