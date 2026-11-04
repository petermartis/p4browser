#!/usr/bin/env python3
"""Fetch additional Debian bookworm libs Chrome still needs (libXi, NSS/NSPR, glib, etc.)."""
import os, tarfile, urllib.request, io, sys

POOL = "http://deb.debian.org/debian/pool/main/"
OUT = "/volume1/hermes/renderer/lib"

# pool paths (bookworm) for packages commonly required by chrome-headless-shell
PKGS = {
  "libxi6":            "x/libxi/libxi6_1.8-1+b1_amd64.deb",
  "libxrender1":       "x/libxrender/libxrender1_0.9.10-1.1_amd64.deb",
  "libxext6":          "x/libxext/libxext6_1.3.4-1_amd64.deb",
  "libnss3":           "n/nss/libnss3_3.98-1.1_amd64.deb",
  "libnspr4":          "n/nspr/libnspr4_4.35-1_amd64.deb",
  "libglib2.0-0":      "g/glib2.0/libglib2.0-0_2.74.6-2+deb12u4_amd64.deb",
  "libdbus-1-3":       "d/dbus/libdbus-1-3_1.14.10-1~deb12u1_amd64.deb",
  "libexpat1":         "e/expat/libexpat1_2.5.0-1+deb12u2_amd64.deb",
  "libudev1":          "e/systemd/libudev1_252.39-1~deb12u1_amd64.deb",
  "libfreetype6":      "f/freetype/libfreetype6_2.12.1+dfsg-1+deb12u4_amd64.deb",
  "libfontconfig1":    "f/fontconfig/libfontconfig1_2.14.1-4_amd64.deb",
  "libvulkan1":        "v/vulkan-loader/libvulkan1_1.3.239.0-1_amd64.deb",
  "libcups2":          "c/cups/libcups2_2.4.2-3+deb12u5_amd64.deb",
  "libx11-6":          "x/libx11/libx11-6_2_1.8.4-2+deb12u2_amd64.deb",
  "libxcb1":           "x/libxcb/libxcb1_1.15-1_amd64.deb",
}

def ar_members(data):
    if data[:8] != b"!<arch>\n": return []
    off=8; out=[]
    while off+60 <= len(data):
        hdr=data[off:off+60]
        name=hdr[0:16].decode(errors="replace").strip()
        size=int(hdr[48:58].decode(errors="replace").strip() or 0)
        out.append((name,off+60,size))
        off=off+60+size+(size%2)
    return out

have = set(os.listdir(OUT))
added = []
for name, pool in PKGS.items():
    url = POOL + pool
    try:
        req = urllib.request.Request(url, headers={"User-Agent":"Mozilla/5.0"})
        data = urllib.request.urlopen(req, timeout=90).read()
    except Exception as e:
        print("DL_FAIL", name, e); continue
    try:
        dt = None
        for nm,start,size in ar_members(data):
            if nm.startswith("data.tar"): dt=data[start:start+size]; break
        if dt is None: print("NO_DATA", name); continue
        got = []
        with tarfile.open(fileobj=io.BytesIO(dt), mode="r:*") as tf:
            for m in tf.getmembers():
                base=os.path.basename(m.name)
                if base.startswith("lib") and ".so" in base and m.isfile():
                    dest=os.path.join(OUT, base)
                    if os.path.exists(dest): continue
                    src=tf.extractfile(m)
                    if src is None: continue
                    with open(dest,"wb") as o: o.write(src.read())
                    got.append(base)
        added += got
        print("ok", name, got)
    except Exception as e:
        print("EXTRACT_ERR", name, e)
print("TOTAL ADDED:", len(added))
