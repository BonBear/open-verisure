#!/usr/bin/env python3
"""Extract one file from a remote ZIP archive with HTTP range requests.

Used by CI to take librail_efr32xg14_gcc_release.a out of the Gecko SDK release asset
gecko-sdk.zip (about 1.3 GB) without downloading the whole archive: it reads the end of central
directory, finds the member, then fetches only its local header and compressed data.
The caller checks the SHA-256 of the result.

    fetch_zip_member.py <url> <member path inside the zip> <output file>
"""
import struct
import sys
import time
import urllib.request
import zlib

EOCD_SIG = b"PK\x05\x06"
EOCD64_LOC_SIG = b"PK\x06\x07"
EOCD64_SIG = b"PK\x06\x06"
CDIR_SIG = b"PK\x01\x02"
LOCAL_SIG = b"PK\x03\x04"
MAX_MEMBER = 64 * 1024 * 1024  # sanity limit for the extracted file
MAX_CDIR = 256 * 1024 * 1024   # sanity limit for the central directory
ATTEMPTS = 3


def retry(fn, *args):
    for i in range(ATTEMPTS):
        try:
            return fn(*args)
        except (OSError, RuntimeError) as e:  # URLError, IncompleteRead, timeouts, short reads
            if i == ATTEMPTS - 1:
                raise
            print(f"retry after: {e}", file=sys.stderr)
            time.sleep(5 * (i + 1))


def _fetch(url, start, length):
    req = urllib.request.Request(url, headers={"Range": f"bytes={start}-{start + length - 1}"})
    with urllib.request.urlopen(req, timeout=120) as r:
        if r.status != 206:
            raise RuntimeError(f"server ignored the range request (HTTP {r.status})")
        data = r.read()
    if len(data) != length:
        raise RuntimeError(f"short read: {len(data)} of {length} bytes")
    return data


def fetch(url, start, length):
    return retry(_fetch, url, start, length)


def _total_size(url):
    req = urllib.request.Request(url, headers={"Range": "bytes=0-0"})
    with urllib.request.urlopen(req, timeout=120) as r:
        rng = r.headers.get("Content-Range", "")
    if "/" not in rng:
        raise RuntimeError("no Content-Range in the reply")
    return int(rng.rsplit("/", 1)[1])


def total_size(url):
    return retry(_total_size, url)


def central_directory(url, size):
    tail_len = min(size, 65536 + 22 + 20)
    tail = fetch(url, size - tail_len, tail_len)
    i = tail.rfind(EOCD_SIG)
    if i < 0:
        raise RuntimeError("end of central directory not found")
    cd_size, cd_off = struct.unpack_from("<II", tail, i + 12)
    if cd_off == 0xFFFFFFFF or cd_size == 0xFFFFFFFF:  # ZIP64
        j = tail.rfind(EOCD64_LOC_SIG, 0, i)
        if j < 0:
            raise RuntimeError("ZIP64 locator not found")
        (eocd64_off,) = struct.unpack_from("<Q", tail, j + 8)
        rec = fetch(url, eocd64_off, 56)
        if rec[:4] != EOCD64_SIG:
            raise RuntimeError("bad ZIP64 end of central directory")
        cd_size, cd_off = struct.unpack_from("<QQ", rec, 40)
    if cd_size > MAX_CDIR:
        raise RuntimeError(f"central directory too large ({cd_size} bytes)")
    return fetch(url, cd_off, cd_size)


def find_member(cd, name):
    want = name.encode()
    p = 0
    while p + 46 <= len(cd):
        if cd[p:p + 4] != CDIR_SIG:
            raise RuntimeError("bad central directory entry")
        method, = struct.unpack_from("<H", cd, p + 10)
        crc, csize, usize = struct.unpack_from("<III", cd, p + 16)
        nlen, xlen, clen = struct.unpack_from("<HHH", cd, p + 28)
        (loff,) = struct.unpack_from("<I", cd, p + 42)
        fname = cd[p + 46:p + 46 + nlen]
        if fname == want:
            extra = cd[p + 46 + nlen:p + 46 + nlen + xlen]
            q = 0
            while q + 4 <= len(extra):  # ZIP64 extended information
                hid, hlen = struct.unpack_from("<HH", extra, q)
                if hid == 0x0001:
                    vals = list(struct.unpack_from("<" + "Q" * (hlen // 8), extra, q + 4))
                    if usize == 0xFFFFFFFF:
                        usize = vals.pop(0)
                    if csize == 0xFFFFFFFF:
                        csize = vals.pop(0)
                    if loff == 0xFFFFFFFF:
                        loff = vals.pop(0)
                q += 4 + hlen
            return method, crc, csize, usize, loff
        p += 46 + nlen + xlen + clen
    raise RuntimeError(f"{name} not in the archive")


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    url, name, out = sys.argv[1:]
    if not url.startswith("https://"):
        sys.exit("only https:// URLs are accepted")
    size = total_size(url)
    method, crc, csize, usize, loff = find_member(central_directory(url, size), name)
    if usize > MAX_MEMBER or csize > MAX_MEMBER:
        raise RuntimeError(f"member too large ({usize} bytes)")
    hdr = fetch(url, loff, 30)
    if hdr[:4] != LOCAL_SIG:
        raise RuntimeError("bad local file header")
    nlen, xlen = struct.unpack_from("<HH", hdr, 26)
    comp = fetch(url, loff + 30 + nlen + xlen, csize) if csize else b""
    if method == 0:
        data = comp
    elif method == 8:
        data = zlib.decompressobj(-15).decompress(comp, usize + 1)  # capped output
    else:
        raise RuntimeError(f"unsupported compression method {method}")
    if len(data) != usize or zlib.crc32(data) != crc:
        raise RuntimeError("size or CRC-32 mismatch")
    with open(out, "wb") as f:
        f.write(data)
    print(f"{name}: {usize} bytes from {csize} compressed")


if __name__ == "__main__":
    main()
