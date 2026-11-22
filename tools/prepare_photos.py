#!/usr/bin/env python3
"""prepare_photos.py: make device photos safe and small enough to commit.

    python tools/prepare_photos.py IMG_0001.jpg photos/ --out clean
    python tools/prepare_photos.py photos/ --rename IMG_0001.jpg=siren_front.jpg
    python tools/prepare_photos.py photos/ --rename names.txt --in-place

For every input JPEG (files, or directories scanned for *.jpg / *.jpeg): applies the EXIF
orientation, drops ALL metadata (EXIF incl. GPS, XMP, ICC profile), scales the long side down
to --max-side (never up) with LANCZOS and saves a JPEG with --quality. Prints name, size
before and after, and whether the original carried GPS data. Exits non-zero if an output
still has EXIF.

Output goes to --out (default: next to the input, file name suffixed `_clean`; the originals
stay, with their GPS data, so delete them yourself). An input is never overwritten unless
--in-place is given; with --in-place and --rename the original is removed after the renamed
copy was written. An output that equals any input, or one already written in the same run
(same name from two folders), is refused. Inputs already named `*_clean` are skipped in the
default mode. --rename takes old=new pairs (repeatable) or
the path of a mapping file with one `old new` pair per line (# comments allowed); names are
matched on the input file name.

Requires Pillow (pip install -r requirements.txt).
"""
import argparse
import os
import sys

try:
    from PIL import Image, ImageOps
except ImportError:  # pragma: no cover
    sys.exit("[-] Pillow is required: pip install Pillow")

GPS_IFD = 0x8825
JPEG_EXT = (".jpg", ".jpeg")


def collect(inputs):
    files = []
    for p in inputs:
        if os.path.isdir(p):
            for n in sorted(os.listdir(p)):
                if n.lower().endswith(JPEG_EXT):
                    files.append(os.path.join(p, n))
        elif os.path.isfile(p):
            files.append(p)
        else:
            print(f"[!] not found, skipped: {p}", file=sys.stderr)
    return files


def load_renames(specs):
    m = {}
    for s in specs or []:
        if "=" in s:
            old, new = s.split("=", 1)
            m[old.strip()] = new.strip()
        elif os.path.isfile(s):
            with open(s, encoding="utf-8") as f:
                for ln in f:
                    ln = ln.strip()
                    if not ln or ln.startswith("#"):
                        continue
                    parts = ln.split(None, 1)
                    if len(parts) == 2:
                        m[parts[0]] = parts[1].strip()
                    else:
                        print(f"[!] bad mapping line skipped: {ln}", file=sys.stderr)
        else:
            raise SystemExit(f"[-] --rename: '{s}' is neither old=new nor an existing file")
    return m


def has_exif(path):
    """True if the JPEG still carries metadata: EXIF, XMP, ICC, a comment or any APPn marker
    other than the JFIF APP0."""
    with Image.open(path) as im:
        applist = getattr(im, "applist", [])
        return (bool(im.getexif()) or any(m != "APP0" for m, _ in applist)
                or any(k in im.info for k in ("exif", "xmp", "XML:com.adobe.xmp", "icc_profile", "comment")))


def had_gps(im):
    try:
        return bool(im.getexif().get_ifd(GPS_IFD))
    except Exception:  # damaged EXIF: treat as not readable
        return False


def process(src, dst, max_side, quality):
    """Write the cleaned copy of src to dst; returns (size_before, size_after, gps_present)."""
    before = os.path.getsize(src)
    with Image.open(src) as im:
        gps = had_gps(im)
        im = ImageOps.exif_transpose(im)
        im = im.convert("RGB")
        w, h = im.size
        long_side = max(w, h)
        if long_side > max_side:
            s = max_side / long_side
            im = im.resize((max(1, round(w * s)), max(1, round(h * s))), Image.LANCZOS)
        # a fresh image has no info/exif; save without exif/icc/xmp arguments
        clean = Image.new("RGB", im.size)
        clean.paste(im)
        tmp = dst + ".tmp"
        try:
            clean.save(tmp, "JPEG", quality=quality, optimize=True)
        except BaseException:
            if os.path.exists(tmp):
                os.remove(tmp)
            raise
    os.replace(tmp, dst)
    return before, os.path.getsize(dst), gps


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+", help="JPEG files or directories")
    ap.add_argument("--out", help="output directory (default: next to the input, suffix _clean)")
    ap.add_argument("--max-side", type=int, default=3264, help="longest side in pixels (default 3264)")
    ap.add_argument("--quality", type=int, default=85, help="JPEG quality (default 85)")
    ap.add_argument("--rename", action="append", metavar="OLD=NEW|FILE",
                    help="rename pair or mapping file ('old new' per line); repeatable")
    ap.add_argument("--in-place", action="store_true", help="overwrite the inputs")
    a = ap.parse_args(argv)
    if a.max_side < 1 or not 1 <= a.quality <= 100:
        ap.error("--max-side must be >= 1 and --quality 1..100")
    if a.in_place and a.out:
        ap.error("--in-place and --out exclude each other")
    ren = load_renames(a.rename)
    files = collect(a.inputs)
    if not files:
        print("[-] no input files", file=sys.stderr)
        return 1
    if a.out:
        os.makedirs(a.out, exist_ok=True)
    key = lambda p: os.path.normcase(os.path.realpath(p))  # noqa: E731
    input_keys = {key(f) for f in files}
    written = set()
    bad = 0
    for src in files:
        base = os.path.basename(src)
        name = ren.get(base, base)
        stem, ext = os.path.splitext(name)
        if a.in_place:
            dst = os.path.join(os.path.dirname(src), name)
        else:
            if not a.out and not ren and stem.endswith("_clean"):
                print(f"[!] {base}: already a _clean output, skipped", file=sys.stderr)
                continue
            outdir = a.out or os.path.dirname(src)
            dst = os.path.join(outdir, name if a.out else f"{stem}_clean{ext or '.jpg'}")
        dk = key(dst)
        if dk in input_keys and not (a.in_place and dk == key(src)):
            print(f"[-] {base}: output {dst} is another input, refused", file=sys.stderr)
            bad += 1
            continue
        if dk in written:
            print(f"[-] {base}: output {dst} was already written in this run, refused", file=sys.stderr)
            bad += 1
            continue
        try:
            before, after, gps = process(src, dst, a.max_side, a.quality)
            ok = not has_exif(dst)
        except Exception as e:  # one unreadable or unwritable file must not stop the batch
            print(f"[-] {base}: {e}", file=sys.stderr)
            bad += 1
            continue
        written.add(dk)
        print(f"{base} -> {os.path.basename(dst)}: {before} -> {after} bytes, "
              f"GPS {'PRESENT in original' if gps else 'absent'}"
              + ("" if ok else ", [-] OUTPUT STILL HAS METADATA"))
        if not ok:
            bad += 1
        elif a.in_place and dk != key(src):
            os.remove(src)  # renamed in place: the original (with its metadata) must not stay
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
