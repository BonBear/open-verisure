"""release/make_release.py: the package built from HEAD and relink(). The --check builds
(SDCC, PlatformIO) are not run here."""
import os
import zipfile

import pytest

import make_release


@pytest.fixture(scope="module")
def package(tmp_path_factory):
    out = tmp_path_factory.mktemp("dist")
    zip_path, name = make_release.build("HEAD", str(out))
    with zipfile.ZipFile(zip_path) as z:
        names = z.namelist()
    return zip_path, name, names


def test_zip_name(package):
    zip_path, name, _ = package
    assert os.path.basename(zip_path) == name + ".zip"
    assert name.startswith("open-verisure-")


def test_no_secret_files(package):
    _, _, names = package
    assert not [n for n in names if os.path.basename(n) in ("secrets.h", "config.h")]


def test_all_under_prefix(package):
    _, name, names = package
    assert names and all(n.startswith(name + "/") for n in names)


@pytest.mark.parametrize("path", [
    "README.md", "LICENSE", "requirements.txt", "derive_key.py", "cc1110_tools.py",
    "868Receiver/platformio.ini", "868Receiver/src/secrets.h.example",
    "868Receiver/lib/GuardedWebServer/src/GuardedWebServer.h",
    "VerisureDoorSensor/main.c", "VerisureDoorSensor/config.h.example",
    "VerisureDoorSensor/build_and_flash.py", "MD_CAM/build_and_flash_md_cam.py",
    "MD_CAM/firmware/config.h.example",
    "SIREN/firmware/platformio.ini", "SIREN/firmware/src/main.c",
    "tools/esp32_msp430_bsl/bsl_flash.py",
])
def test_expected_files(package, path):
    _, name, names = package
    assert f"{name}/{path}" in names


@pytest.mark.parametrize("d", [
    "cc1110_common/", "documentation/", "868Receiver/src/", "868Receiver/lib/GuardedWebServer/",
    "MD_CAM/blackfin/",
])
def test_expected_dirs(package, d):
    _, name, names = package
    assert any(n.startswith(f"{name}/{d}") for n in names)


@pytest.mark.parametrize("path", [
    "TODO.md", "AGENTS.md", "SIREN/docs/",
    "MD_CAM/docs/", "VerisureDoorSensor/docs/", ".github/", "tests/", "release/",
])
def test_left_out(package, path):
    _, name, names = package
    assert not any(n.startswith(f"{name}/{path}") for n in names)


def test_relink(tmp_path):
    top = tmp_path / "pkg"
    (top / "docs").mkdir(parents=True)
    (top / "present.md").write_text("x", encoding="utf-8")
    md = top / "docs" / "guide.md"
    with open(md, "w", encoding="utf-8", newline="") as f:
        f.write("[ok](../present.md) [gone](../TODO.md#top) [web](https://example.com/a.md) "
                "[mail](mailto:a@b.c) [out](../../outside.md) [anchor](#here) [img](pic.png)\r\n")
    blob = "https://github.com/o/r/blob/abc/"
    n = make_release.relink(str(top), blob)
    with open(md, encoding="utf-8", newline="") as f:
        s = f.read()
    assert n == 2
    assert "[ok](../present.md)" in s
    assert f"[gone]({blob}TODO.md#top)" in s
    assert f"[img]({blob}docs/pic.png)" in s
    assert "[web](https://example.com/a.md)" in s and "[mail](mailto:a@b.c)" in s
    assert "[out](../../outside.md)" in s and "[anchor](#here)" in s
    assert s.endswith("\r\n")  # line endings kept
    assert make_release.broken_links(str(top)) == [os.path.join("docs", "guide.md") + " -> ../../outside.md"]


def test_relink_without_remote(tmp_path):
    (tmp_path / "a.md").write_text("[gone](missing.md)", encoding="utf-8")
    assert make_release.relink(str(tmp_path), None) == 0
    assert (tmp_path / "a.md").read_text(encoding="utf-8") == "[gone](missing.md)"
