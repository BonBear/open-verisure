"""tools/prepare_photos.py: metadata stripped, orientation applied, size limited, input kept."""
import os
import sys

import pytest

Image = pytest.importorskip("PIL.Image")

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import prepare_photos as pp  # noqa: E402

GPS_IFD = 0x8825


def make_jpeg(path, size=(400, 200), orientation=6, gps=True):
    im = Image.new("RGB", size, (200, 40, 40))
    exif = Image.Exif()
    exif[0x0112] = orientation
    exif[0x010F] = "TestCam"
    if gps:
        exif[GPS_IFD] = {1: "N", 2: (0.0, 0.0, 1.0), 3: "E", 4: (0.0, 0.0, 1.0)}  # synthetic 0:0:1 N/E
    im.save(path, "JPEG", exif=exif.tobytes())
    return path


def exif_of(path):
    with Image.open(path) as im:
        return dict(im.getexif()), im.getexif().get_ifd(GPS_IFD), im.size


def test_strip_rotate_downscale(tmp_path, capsys):
    src = make_jpeg(tmp_path / "IMG_1.jpg")
    before = src.read_bytes()
    tags, gps, _ = exif_of(src)
    assert tags.get(0x0112) == 6 and gps          # the fixture really has EXIF + GPS
    out = tmp_path / "clean"
    assert pp.main([str(src), "--out", str(out), "--max-side", "100"]) == 0
    dst = out / "IMG_1.jpg"
    tags, gps, size = exif_of(dst)
    assert tags == {} and not gps
    assert size == (50, 100)                       # 400x200 rotated by orientation 6, then scaled
    assert src.read_bytes() == before              # input untouched
    assert "GPS PRESENT" in capsys.readouterr().out


def test_no_upscale_and_default_name(tmp_path):
    src = make_jpeg(tmp_path / "a.jpg", size=(60, 40), orientation=1, gps=False)
    assert pp.main([str(src)]) == 0
    dst = tmp_path / "a_clean.jpg"
    assert exif_of(dst)[2] == (60, 40)
    assert src.exists()


def test_directory_and_rename(tmp_path, capsys):
    d = tmp_path / "in"
    d.mkdir()
    make_jpeg(d / "IMG_1.jpg")
    make_jpeg(d / "IMG_2.JPG", orientation=1)
    (d / "notes.txt").write_text("x")
    m = tmp_path / "names.txt"
    m.write_text("# map\nIMG_2.JPG siren_back.jpg\n", encoding="utf-8")
    out = tmp_path / "out"
    assert pp.main([str(d), "--out", str(out), "--rename", "IMG_1.jpg=siren_front.jpg",
                    "--rename", str(m)]) == 0
    assert sorted(os.listdir(out)) == ["siren_back.jpg", "siren_front.jpg"]
    assert sorted(os.listdir(d)) == ["IMG_1.jpg", "IMG_2.JPG", "notes.txt"]


def test_in_place(tmp_path):
    src = make_jpeg(tmp_path / "p.jpg")
    assert pp.main([str(src), "--in-place"]) == 0
    tags, gps, size = exif_of(src)
    assert tags == {} and not gps and size == (200, 400)


def test_bad_args_and_missing(tmp_path, capsys):
    assert pp.main([str(tmp_path / "none.jpg")]) == 1
    src = make_jpeg(tmp_path / "q.jpg")
    with pytest.raises(SystemExit):
        pp.main([str(src), "--in-place", "--out", str(tmp_path / "o")])
    bad = tmp_path / "bad.jpg"
    bad.write_bytes(b"not a jpeg")
    assert pp.main([str(bad)]) == 1               # unreadable image: reported, non-zero
    assert not (tmp_path / "bad_clean.jpg").exists()


def _segment(marker, payload):
    return b"\xff" + bytes([marker]) + (len(payload) + 2).to_bytes(2, "big") + payload


def inject_metadata(path):
    """Insert an XMP (APP1), an ICC (APP2) and a comment (COM) segment right after SOI."""
    raw = path.read_bytes()
    segs = (_segment(0xE1, b"http://ns.adobe.com/xmp/1.0/\0<x:xmpmeta>secret</x:xmpmeta>")
            + _segment(0xE2, b"ICC_PROFILE\0\x01\x01" + b"\0" * 32)
            + _segment(0xFE, b"secret comment"))
    path.write_bytes(raw[:2] + segs + raw[2:])


def test_xmp_icc_comment_removed(tmp_path):
    src = make_jpeg(tmp_path / "m.jpg")
    inject_metadata(src)
    with Image.open(src) as im:                    # the fixture really carries them
        markers = [m for m, _ in im.applist]
        assert "APP1" in markers and "APP2" in markers and "comment" in im.info
    assert pp.has_exif(src)
    out = tmp_path / "o"
    assert pp.main([str(src), "--out", str(out)]) == 0
    with Image.open(out / "m.jpg") as im:
        assert [m for m, _ in im.applist] == ["APP0"]
        assert "comment" not in im.info and "icc_profile" not in im.info
    assert not pp.has_exif(out / "m.jpg")


def test_in_place_rename_removes_original(tmp_path):
    src = make_jpeg(tmp_path / "IMG_9.jpg")
    assert pp.main([str(src), "--in-place", "--rename", "IMG_9.jpg=front.jpg"]) == 0
    assert not src.exists()
    assert exif_of(tmp_path / "front.jpg")[0] == {}


def test_refuses_output_that_is_another_input(tmp_path):
    a = make_jpeg(tmp_path / "a.jpg", gps=False)
    c = make_jpeg(tmp_path / "a_clean.jpg", gps=False)
    before = c.read_bytes()
    assert pp.main([str(tmp_path)]) == 1           # a.jpg would overwrite a_clean.jpg
    assert c.read_bytes() == before and a.exists()
    assert not (tmp_path / "a_clean_clean.jpg").exists()


def test_clean_inputs_skipped_in_default_mode(tmp_path):
    make_jpeg(tmp_path / "b_clean.jpg", gps=False)
    assert pp.main([str(tmp_path)]) == 0
    assert sorted(os.listdir(tmp_path)) == ["b_clean.jpg"]


def test_same_basename_from_two_folders_refused(tmp_path):
    for d in ("d1", "d2"):
        (tmp_path / d).mkdir()
        make_jpeg(tmp_path / d / "IMG_0001.jpg", size=(40 + len(d), 20), gps=False)
    out = tmp_path / "out"
    assert pp.main([str(tmp_path / "d1"), str(tmp_path / "d2"), "--out", str(out)]) == 1
    assert os.listdir(out) == ["IMG_0001.jpg"]


def test_save_failure_cleans_tmp_and_batch_continues(tmp_path, monkeypatch, capsys):
    d = tmp_path / "in"
    d.mkdir()
    make_jpeg(d / "1.jpg")
    make_jpeg(d / "2.jpg")
    real = Image.Image.save
    calls = []

    def flaky(self, fp, *a, **k):
        calls.append(fp)
        if len(calls) == 1:
            with open(fp, "wb") as f:
                f.write(b"partial")
            raise OSError("disk full")
        return real(self, fp, *a, **k)

    monkeypatch.setattr(Image.Image, "save", flaky)
    out = tmp_path / "o"
    assert pp.main([str(d), "--out", str(out)]) == 1
    assert sorted(os.listdir(out)) == ["2.jpg"]    # no 1.jpg.tmp left, second file done
    assert "disk full" in capsys.readouterr().err


def test_exif_left_in_output_fails(tmp_path, monkeypatch):
    src = make_jpeg(tmp_path / "e.jpg")
    monkeypatch.setattr(pp, "has_exif", lambda p: True)
    assert pp.main([str(src), "--out", str(tmp_path / "o")]) == 1
