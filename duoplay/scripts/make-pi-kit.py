"""Builds the Raspberry Pi SD-card kit.

The kit is a zip whose contents you copy onto a FAT32 microSD card (the iPhone's Files app can
do it). The Pi then boots Alpine Linux from memory and runs DuoPlay, with a Wi-Fi setup hotspot
on first start. No disk image to flash, so no computer needed.

    python3 scripts/make-pi-kit.py --bridge path/to/duoplay-bridge-armv6 --out dist [--image]

--image also writes a raw SD-card image (MBR + FAT32), used to boot-test the kit in QEMU.
Alpine's Raspberry Pi release and its hostapd package are downloaded once, checksummed and
used unmodified; files only a Pi 4 needs, and packages DuoPlay doesn't use, are left out.
"""
import argparse
import gzip
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

ALPINE = ("https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/armhf/alpine-rpi-3.24.4-armhf.tar.gz",
          "09a46351bc414232f34511fef95b16596522d7ed6e15d1b555656b2d828dc038")
HOSTAPD = ("https://dl-cdn.alpinelinux.org/alpine/v3.24/main/armhf/hostapd-2.11-r4.apk",
           "174834a063e6bfe49d4ce49f8e91a74492307fc470ad80eec75cf376a6673556")

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.join(HERE, "..", "pi", "kit")
STAMP = (2025, 1, 1, 0, 0, 0)  # fixed timestamps: the same inputs give the same zip

# Only a Pi 4 / 400 / CM4 needs these; this kit is for the Pi Zero W and its 32-bit relatives.
SKIP = re.compile(r"^(start4\.elf|fixup4\.dat|bcm2711-.*\.dtb|boot/System\.map-.*|boot/config-.*)$")
OVERLAYS = {"disable-bt.dtbo", "miniuart-bt.dtbo", "dwc2.dtbo", "overlay_map.dtb", "hat_map.dtb"}


def fetch(url, sha256, cache):
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, url.rsplit("/", 1)[1])
    if not os.path.exists(path) or hashlib.sha256(open(path, "rb").read()).hexdigest() != sha256:
        print("downloading", url)
        with urllib.request.urlopen(url) as r, open(path + ".part", "wb") as f:
            shutil.copyfileobj(r, f)
        os.replace(path + ".part", path)
    digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
    if digest != sha256:
        sys.exit(f"{path}: checksum mismatch ({digest})")
    return path


def apk_index(raw):
    """APKINDEX.tar.gz -> {name: entry} with deps, provides and install_if parsed."""
    with tarfile.open(fileobj=io.BytesIO(raw)) as t:
        text = t.extractfile("APKINDEX").read().decode()
    pkgs, provides = {}, {}
    for block in text.split("\n\n"):
        f = {}
        for line in block.splitlines():
            if len(line) > 2 and line[1] == ":":
                f[line[0]] = line[2:]
        if "P" not in f:
            continue
        e = {"name": f["P"], "file": f"{f['P']}-{f['V']}.apk", "deps": f.get("D", "").split(),
             "install_if": f.get("i", "").split()}
        pkgs[e["name"]] = e
        provides.setdefault(e["name"], e["name"])
        for p in f.get("p", "").split():
            provides.setdefault(re.split("[=<>~]", p)[0], e["name"])
    return pkgs, provides


def closure(roots, pkgs, provides):
    """What apk installs for `roots`: dependencies, plus packages whose install_if is met."""
    want = set()

    def add(dep):
        if dep.startswith("!"):
            return
        name = provides.get(re.split("[=<>~]", dep)[0])
        if name is None:
            sys.exit(f"package {dep} isn't in Alpine's boot repository")
        if name not in want:
            want.add(name)
            for d in pkgs[name]["deps"]:
                add(d)

    for r in roots:
        add(r)
    while True:
        more = [p["name"] for p in pkgs.values() if p["install_if"] and p["name"] not in want and all(
            provides.get(re.split("[=<>~]", c)[0]) in want for c in p["install_if"])]
        if not more:
            return want
        for m in more:
            add(m)


def apkovl(hostapd_apk):
    """DuoPlay's overlay: Alpine applies it to the system it builds in memory at every boot."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz", format=tarfile.USTAR_FORMAT) as t:
        def add(name, data=None, mode=0o644, link=None, kind=tarfile.REGTYPE):
            info = tarfile.TarInfo(name)
            info.uid = info.gid = 0
            info.uname = info.gname = "root"
            info.mtime = 1735689600
            info.mode = mode
            info.type = kind
            if link:
                info.linkname = link
            if data is not None:
                info.size = len(data)
                t.addfile(info, io.BytesIO(data))
            else:
                t.addfile(info)

        root = os.path.join(KIT, "overlay")
        dirs = set()
        entries = []
        for base, _, names in os.walk(root):
            for n in names:
                full = os.path.join(base, n)
                rel = os.path.relpath(full, root).replace(os.sep, "/")
                entries.append((rel, full))
        entries.append(("etc/runlevels/default/duoplay", None))
        entries.append(("usr/local/sbin/hostapd", None))
        for rel, _ in entries:
            parts = rel.split("/")[:-1]
            for i in range(1, len(parts) + 1):
                dirs.add("/".join(parts[:i]))
        for d in sorted(dirs):
            add(d, mode=0o755, kind=tarfile.DIRTYPE)
        executable = re.compile(r"^(etc/init\.d/|usr/sbin/|usr/local/sbin/)")
        for rel, full in sorted(entries):
            if rel == "etc/runlevels/default/duoplay":
                add(rel, link="/etc/init.d/duoplay", mode=0o777, kind=tarfile.SYMTYPE)
            elif rel == "usr/local/sbin/hostapd":
                with tarfile.open(hostapd_apk, "r:gz") as h:
                    add(rel, h.extractfile("usr/sbin/hostapd").read(), 0o755)
            else:
                data = open(full, "rb").read()
                add(rel, data, 0o755 if executable.match(rel) else 0o644)
    # gzip with a fixed timestamp, for a reproducible zip
    raw = buf.getvalue()
    out = io.BytesIO()
    with gzip.GzipFile(fileobj=out, mode="wb", mtime=0) as g:
        g.write(gzip.decompress(raw))
    return out.getvalue()


def collect(bridge, cache):
    alpine = fetch(*ALPINE, cache)
    hostapd = fetch(*HOSTAPD, cache)
    files = {}
    with tarfile.open(alpine, "r:gz") as t:
        members = [m for m in t.getmembers() if m.isfile()]
        index = next(m for m in members if m.name.endswith("apks/armhf/APKINDEX.tar.gz"))
        pkgs, provides = apk_index(t.extractfile(index).read())
        world = open(os.path.join(KIT, "overlay", "etc", "apk", "world")).read().split()
        # alpine-base is always added; openssl when the initramfs checks the modloop's signature.
        keep = {pkgs[n]["file"] for n in closure(world + ["alpine-base", "openssl"], pkgs, provides)}
        for m in members:
            name = m.name[2:] if m.name.startswith("./") else m.name
            if SKIP.match(name):
                continue
            if name.startswith("overlays/") and name.split("/", 1)[1] not in OVERLAYS:
                continue
            if name.startswith("apks/armhf/") and name.endswith(".apk") and name.rsplit("/", 1)[1] not in keep:
                continue
            files[name] = t.extractfile(m).read()
    files["duoplay.apkovl.tar.gz"] = apkovl(hostapd)
    for base, _, names in os.walk(os.path.join(KIT, "card")):
        for n in names:
            full = os.path.join(base, n)
            files[os.path.relpath(full, os.path.join(KIT, "card")).replace(os.sep, "/")] = open(full, "rb").read()
    files["duoplay/duoplay-bridge"] = open(bridge, "rb").read()
    return files, sorted(os.path.basename(f) for f in keep)


def write_zip(files, path):
    with zipfile.ZipFile(path + ".part", "w") as z:
        for name in sorted(files):
            info = zipfile.ZipInfo(name, STAMP)
            info.external_attr = (0o755 if name.endswith(("duoplay-bridge", ".elf", ".bin")) else 0o644) << 16
            # gz/apk files don't shrink; storing them keeps unzipping on the phone quick
            info.compress_type = zipfile.ZIP_STORED if name.endswith((".gz", ".apk")) or "modloop" in name \
                else zipfile.ZIP_DEFLATED
            z.writestr(info, files[name])
    os.replace(path + ".part", path)


def write_image(files, path, size_mb=256):
    """A raw SD-card image: MBR with one FAT32 partition, like a freshly formatted card."""
    with tempfile.TemporaryDirectory() as tmp:
        stage = os.path.join(tmp, "card")
        for name, data in files.items():
            dest = os.path.join(stage, name)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            open(dest, "wb").write(data)
        part = os.path.join(tmp, "part.img")
        start = 2048
        sectors = size_mb * 2048 - start
        with open(part, "wb") as f:
            f.truncate(sectors * 512)
        subprocess.run(["mkfs.vfat", "-F", "32", "-n", "DUOPLAY", part], check=True, stdout=subprocess.DEVNULL)
        entries = [os.path.join(stage, n) for n in os.listdir(stage)]
        subprocess.run(["mcopy", "-s", "-i", part] + entries + ["::/"], check=True)
        with open(path, "wb") as img:
            img.truncate(size_mb * 1024 * 1024)
        subprocess.run(["sfdisk", "--quiet", path], input=f"label: dos\nstart={start}, type=c, bootable\n",
                       text=True, check=True)
        with open(path, "r+b") as img, open(part, "rb") as p:
            img.seek(start * 512)
            shutil.copyfileobj(p, img)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bridge", required=True, help="the ARMv6 duoplay-bridge binary (scripts/build-pi.sh)")
    ap.add_argument("--out", required=True, help="output folder")
    ap.add_argument("--cache", default=os.path.expanduser("~/.cache/duoplay-pi"))
    ap.add_argument("--image", action="store_true", help="also write a raw SD-card image for testing")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    files, apks = collect(a.bridge, a.cache)
    zpath = os.path.join(a.out, "duoplay-pi-kit.zip")
    write_zip(files, zpath)
    total = sum(len(d) for d in files.values())
    print(f"{zpath}: {len(files)} files, {total / 1e6:.1f} MB on the card, {os.path.getsize(zpath) / 1e6:.1f} MB zipped")
    print(f"  packages: {len(apks)}")
    if a.image:
        ipath = os.path.join(a.out, "duoplay-pi.img")
        write_image(files, ipath)
        print(f"{ipath}: test image")


if __name__ == "__main__":
    main()
