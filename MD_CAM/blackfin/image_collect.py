"""Fetch the camera picture that the CC1110 (BF_IMG_LINK) sends over RF, from the gateway.

The CC1110 sends raw grayscale tiles (RF packet type 0x81, 8 pixels + tile number) of a
320x240 picture; the gateway places them by number and serves the picture as an 8-bit
BMP on /api/camera.bmp (header X-Tiles = tiles received / 9600). It also publishes it on
MQTT (<topic>/<sensor id>/image) when complete or after a quiet period.

Usage: python image_collect.py [out.png] [--gateway 192.168.0.142]
"""
import io
import os
import sys
import urllib.request

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    args = sys.argv[1:]
    gw = args[args.index("--gateway") + 1] if "--gateway" in args else "192.168.0.142"
    out = next((a for a in args if a.endswith(".png")), os.path.join(HERE, "build", "camera_320x240.png"))
    with urllib.request.urlopen(f"http://{gw}/api/camera.bmp", timeout=30) as r:
        tiles = r.headers.get("X-Tiles", "?")
        img = Image.open(io.BytesIO(r.read())).convert("L")
    img.save(out)
    print(f"[+] {img.size[0]}x{img.size[1]}, tiles {tiles} -> {out}")


if __name__ == "__main__":
    main()
