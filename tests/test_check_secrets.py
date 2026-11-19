""".github/scripts/check_secrets.py: passes on the repository, fails on a planted key."""
import os
import subprocess
import sys

import pytest

from conftest import ROOT

SCRIPT = os.path.join(ROOT, ".github", "scripts", "check_secrets.py")


def run(cwd):
    return subprocess.run([sys.executable, SCRIPT], cwd=cwd, capture_output=True, text=True)


def test_repository_passes():
    r = run(ROOT)
    assert r.returncode == 0, r.stdout + r.stderr
    assert ", 0 findings" in r.stdout


@pytest.fixture
def repo(tmp_path):
    subprocess.run(["git", "init", "-q", str(tmp_path)], check=True)
    return tmp_path


def add(repo, name, text):
    p = repo / name
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8")
    subprocess.run(["git", "add", name], cwd=repo, check=True)


def planted():
    # built at run time so that this file itself never holds a non-public key
    return "".join(f"{(i * 37 + 11) & 0xFF:02X}" for i in range(16))


def test_allowed_keys_pass(repo):
    add(repo, "a.c", '#define MASTER_KEY_HEX "000102030405060708090A0B0C0D0E0F"\n')
    r = run(repo)
    assert r.returncode == 0, r.stdout


def test_planted_hex_key_fails(repo):
    key = planted()
    add(repo, "notes.md", f"key: {key.lower()}\n")
    r = run(repo)
    assert r.returncode == 1
    assert "notes.md:1:" in r.stdout
    assert key not in r.stdout.upper()  # the value is never printed


def test_planted_c_array_fails(repo):
    b = bytes.fromhex(planted())
    rows = [", ".join(f"0x{x:02X}" for x in b[i:i + 8]) for i in (0, 8)]
    add(repo, "src/k.c", "static const uint8_t DEVICE_AES_KEY[16] = {\n    "
        + ",\n    ".join(rows) + "\n};\n")
    r = run(repo)
    assert r.returncode == 1
    assert "src/k.c:1:" in r.stdout and "16-byte array" in r.stdout


def test_tracked_secrets_h_fails(repo):
    add(repo, "868Receiver/src/secrets.h", "// empty\n")
    r = run(repo)
    assert r.returncode == 1
    assert "generated secret file is tracked" in r.stdout


def test_untracked_key_ignored(repo):
    add(repo, "a.txt", "nothing\n")
    (repo / "loose.txt").write_text(planted(), encoding="utf-8")
    assert run(repo).returncode == 0
