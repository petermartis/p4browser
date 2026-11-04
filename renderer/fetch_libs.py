#!/usr/bin/env python3
"""Fetch Debian bookworm .debs (exact pool paths from pkgmap.json) and extract .so* into ./lib."""
import os, tarfile, urllib.request, io, sys, json

POOL = "http://deb.debian.org/debian/"
OUT = "/volume1/hermes/renderer/lib"
os.makedirs(OUT, exist_ok=True)
pkgmap = json.load(open("/volume1/hermes/renderer/pkgmap.json"))

def ar_members(data):
    if data[:8] != b"!<arch>\n": return []
    off=8; out=[]
    while off+60 <= len(data):
        hdr=data[off:off+60]
        name=hdr[0:16].decode(errors="replace").strip()
        size=int(hdr[48:58].decode(errors="replace").strip() or 0)
        body=off+60
        out.append((name,body,size))
        off=body+size+(size%2)
    return out

extracted=set()
fails=[]
for name,pool in pkgmap.items():
    url=POOL+pool
    try:
        req=urllib.request.Request(url, headers={"User-Agent":"Mozilla/5.0"})
        data=urllib.request.urlopen(req, timeout=120).read()
    except Exception as e:
        fails.append((name,str(e))); print("DL_FAIL", name, e, file=sys.stderr); continue
    try:
        data_tar=None
        for nm,start,size in ar_members(data):
            if nm.startswith("data.tar"):
                data_tar=data[start:start+size]; break
        if data_tar is None:
            fails.append((name,"no data.tar")); continue
        with tarfile.open(fileobj=io.BytesIO(data_tar), mode="r:*") as tf:
            for m in tf.getmembers():
                base=os.path.basename(m.name)
                if base.startswith("lib") and ".so" in base and m.isfile():
                    dest=os.path.join(OUT, base)
                    if os.path.exists(dest): continue
                    src=tf.extractfile(m)
                    if src is None: continue
                    with open(dest,"wb") as o: o.write(src.read())
                    extracted.add(base)
        print("ok", name, file=sys.stderr)
    except Exception as e:
        fails.append((name,"extract:"+str(e))); print("EXTRACT_ERR", name, e, file=sys.stderr)

print("total libs:", len(extracted))
print("fails:", fails)
