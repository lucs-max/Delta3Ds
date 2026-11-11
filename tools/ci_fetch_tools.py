#!/usr/bin/env python3
"""
CI helper: downloads the Linux builds of makerom and bannertool (needed to
pack .cia files) from their GitHub releases into the given folder.

    python3 tools/ci_fetch_tools.py <out dir>
"""
import io
import json
import os
import stat
import sys
import urllib.request
import zipfile


def api(url):
    req = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json"})
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    with urllib.request.urlopen(req) as r:
        return json.load(r)


def download(url):
    with urllib.request.urlopen(url) as r:
        return r.read()


def find_asset(repo, tag_prefix, words):
    for rel in api("https://api.github.com/repos/%s/releases?per_page=50" % repo):
        if tag_prefix and not rel["tag_name"].startswith(tag_prefix):
            continue
        for a in rel["assets"]:
            n = a["name"].lower()
            if all(any(w in n for w in group) for group in words):
                return rel["tag_name"], a["name"], a["browser_download_url"]
    raise SystemExit("no matching release asset in %s" % repo)


def extract(data, want, out_path):
    """Extracts the Linux x86-64 binary called `want` from a zip."""
    z = zipfile.ZipFile(io.BytesIO(data))
    names = [n for n in z.namelist() if os.path.basename(n) == want]
    linux = [n for n in names if "linux" in n.lower() or "ubuntu" in n.lower()] or names
    if not linux:
        raise SystemExit("%s not found in archive: %s" % (want, z.namelist()))
    with open(out_path, "wb") as f:
        f.write(z.read(linux[0]))
    os.chmod(out_path, os.stat(out_path).st_mode | stat.S_IEXEC)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "ci-tools"
    os.makedirs(out, exist_ok=True)

    tag, name, url = find_asset("3DSGuy/Project_CTR", "makerom", [["makerom"], ["ubuntu", "linux"]])
    print("makerom:", tag, name)
    data = download(url)
    if name.endswith(".zip"):
        extract(data, "makerom", os.path.join(out, "makerom"))
    else:
        with open(os.path.join(out, "makerom"), "wb") as f:
            f.write(data)
        os.chmod(os.path.join(out, "makerom"), 0o755)

    tag, name, url = find_asset("Steveice10/bannertool", "", [["bannertool"], [".zip"]])
    print("bannertool:", tag, name)
    extract(download(url), "bannertool", os.path.join(out, "bannertool"))


if __name__ == "__main__":
    main()
