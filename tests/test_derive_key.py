"""derive_key.py: KDF test vector, device ID cleaning, secrets.h parsing, config.h output."""
import sys

import pytest

import cc1110_tools
import derive_key
from conftest import TEST_CHECK_VALUE, TEST_DEVICE_ID, TEST_DEVICE_KEY_HEX, TEST_MASTER_HEX


def test_kdf_test_vector():
    key = derive_key.derive_device_key(bytes.fromhex(TEST_MASTER_HEX), TEST_DEVICE_ID)
    assert key.hex().upper() == TEST_DEVICE_KEY_HEX


def test_check_value():
    assert derive_key.key_check_value(bytes.fromhex(TEST_MASTER_HEX)) == TEST_CHECK_VALUE


def test_master_key_length_checked():
    with pytest.raises(ValueError):
        derive_key.derive_device_key(bytes(15), TEST_DEVICE_ID)


@pytest.mark.parametrize("raw,clean", [
    ("2JHL 2MVN", "2JHL2MVN"),
    ("2jhl-2mvn", "2JHL2MVN"),
    ("  2K79A65A ", "2K79A65A"),
])
def test_clean_device_id_accepts(raw, clean):
    assert derive_key.clean_device_id(raw) == clean


@pytest.mark.parametrize("raw", [
    "2K79A65", "2K79A65AB", "2JHL_2MV", "2JHL.2MV", "2JHL2MV!", "", "ÅJHL2MVN",
])
def test_clean_device_id_rejects(raw):
    with pytest.raises(ValueError):
        derive_key.clean_device_id(raw)


def _secrets(tmp_path, monkeypatch, text):
    p = tmp_path / "secrets.h"
    p.write_text(text, encoding="utf-8")
    monkeypatch.setattr(derive_key, "SECRETS_H_PATH", str(p))


def test_secrets_uncommented_define(tmp_path, monkeypatch):
    _secrets(tmp_path, monkeypatch, f'  #define MASTER_KEY_HEX "{TEST_MASTER_HEX.lower()}"\n')
    assert derive_key.get_master_key_from_secrets() == TEST_MASTER_HEX


@pytest.mark.parametrize("line", [
    '// #define MASTER_KEY_HEX "{k}"',
    '//#define MASTER_KEY_HEX "{k}"',
    ' * #define MASTER_KEY_HEX "{k}"',
    '/* #define MASTER_KEY_HEX "{k}" */',
    'x #define MASTER_KEY_HEX "{k}"',
])
def test_secrets_commented_define_ignored(tmp_path, monkeypatch, line):
    _secrets(tmp_path, monkeypatch, line.format(k=TEST_MASTER_HEX) + "\n")
    assert derive_key.get_master_key_from_secrets() is None


def test_secrets_commented_then_real(tmp_path, monkeypatch):
    example = derive_key.DEFAULT_MASTER_KEY_HEX
    _secrets(tmp_path, monkeypatch,
             f'// #define MASTER_KEY_HEX "{example}"\n#define MASTER_AES_KEY_HEX "{TEST_MASTER_HEX}"\n')
    assert derive_key.get_master_key_from_secrets() == TEST_MASTER_HEX


def test_secrets_missing_or_short(tmp_path, monkeypatch):
    monkeypatch.setattr(derive_key, "SECRETS_H_PATH", str(tmp_path / "none.h"))
    assert derive_key.get_master_key_from_secrets() is None
    _secrets(tmp_path, monkeypatch, f'#define MASTER_KEY_HEX "{TEST_MASTER_HEX[:30]}"\n')
    assert derive_key.get_master_key_from_secrets() is None


def _run_main(monkeypatch, capsys, *args):
    monkeypatch.setattr(sys, "argv", ["derive_key.py", *args])
    derive_key.main()
    return capsys.readouterr().out


def test_main_example_key_warning(tmp_path, monkeypatch, capsys):
    monkeypatch.delenv("VERISURE_MASTER_KEY", raising=False)
    monkeypatch.setattr(derive_key, "SECRETS_H_PATH", str(tmp_path / "none.h"))
    out = _run_main(monkeypatch, capsys, "--id", TEST_DEVICE_ID)
    assert "Built-in fallback default" in out
    assert "PUBLIC example master key" in out
    assert derive_key.DEFAULT_MASTER_KEY_HEX not in out.upper()  # the master key is never printed


def test_main_test_vector_no_warning(tmp_path, monkeypatch, capsys):
    monkeypatch.delenv("VERISURE_MASTER_KEY", raising=False)
    monkeypatch.setattr(derive_key, "SECRETS_H_PATH", str(tmp_path / "none.h"))
    out = _run_main(monkeypatch, capsys, "--master-key", TEST_MASTER_HEX.lower(), "--id", TEST_DEVICE_ID)
    assert f"check value {TEST_CHECK_VALUE}" in out
    assert f"Device Key (Hex): {TEST_DEVICE_KEY_HEX}" in out
    assert "PUBLIC example" not in out


def test_main_key_from_secrets(tmp_path, monkeypatch, capsys):
    monkeypatch.delenv("VERISURE_MASTER_KEY", raising=False)
    _secrets(tmp_path, monkeypatch, f'#define MASTER_KEY_HEX "{TEST_MASTER_HEX}"\n')
    out = _run_main(monkeypatch, capsys, "--id", TEST_DEVICE_ID)
    assert "Key Source      : 868Receiver/src/secrets.h" in out
    assert TEST_DEVICE_KEY_HEX in out


def test_main_rejects_7_char_id(monkeypatch):
    monkeypatch.setattr(sys, "argv", ["derive_key.py", "--master-key", TEST_MASTER_HEX, "--id", "2K79A65"])
    with pytest.raises(SystemExit) as e:
        derive_key.main()
    assert e.value.code != 0


def test_write_config_header_parseable(tmp_path):
    out = tmp_path / "sub" / "config.h"
    key = bytes.fromhex(TEST_DEVICE_KEY_HEX)
    derive_key.write_config_header(TEST_DEVICE_ID, key, str(out))
    text = out.read_text(encoding="utf-8")
    assert cc1110_tools.config_device_id(str(out)) == TEST_DEVICE_ID
    assert "static const __code uint8_t DEVICE_AES_KEY[16]" in text
    body = text.split("DEVICE_AES_KEY[16] = {", 1)[1].split("};", 1)[0]
    assert bytes(int(b, 16) for b in body.replace(",", " ").split()) == key
