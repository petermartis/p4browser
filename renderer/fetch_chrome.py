#!/usr/bin/env python3
"""Download Chrome-for-Testing headless shell and extract without `unzip`."""
import json, os, sys, urllib.request, zipfile, shutil

BASE = "https://storage.googleapis.com/chrome-for-testing-public"
OUT = "/volume1/hermes/renderer/chrome"

def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()

def main():
    # resolve stable version
    ver = None
    try:
        d = json.loads(get("https://googlechromelabs.github.io/chrome-for-testing/last-known-good-versions.json"))
        ver = d["channels"]["Stable"]["version"]
    except Exception as e:
        print("WARN: could not resolve stable version, using known-good:", e)
    if not ver:
        ver = "138.0.7204.96"  # fallback known-good stable
    print("stable version:", ver, file=sys.stderr)

    os.makedirs(OUT, exist_ok=True)
    zipname = f"chrome-headless-shell-{ver}-linux64"
    url = f"{BASE}/{ver}/linux64/{zipname}.zip"
    zpath = f"/volume1/hermes/renderer/{zipname}.zip"
    print("downloading", url, file=sys.stderr)
    with urllib.request.urlopen(url, timeout=300) as r, open(zpath, "wb") as f:
        shutil.copyfileobj(r, f)
    sz = os.path.getsize(zpath)
    print(f"downloaded {sz} bytes", file=sys.stderr)

    print("extracting", file=sys.stderr)
    with zipfile.ZipFile(zpath) as z:
        z.extractall(OUT)
    print("done", file=sys.stderr)
    # print the binary path
    for root, dirs, files in os.walk(OUT):
        for fn in files:
            if fn in ("chrome-headless-shell", "chrome"):
                print(os.path.join(root, fn))

if __name__ == "__main__":
    main()
