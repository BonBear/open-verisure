"""Tests for .claude/hooks/qa_bash_guard.py (qa_tester Bash guard)."""
import json
import subprocess
import sys
from pathlib import Path

import pytest

GUARD = Path(__file__).resolve().parents[1] / ".claude" / "hooks" / "qa_bash_guard.py"


def run_guard(command, agent_type="qa_tester"):
    payload = {"tool_name": "Bash", "tool_input": {"command": command}}
    if agent_type is not None:
        payload["agent_type"] = agent_type
    result = subprocess.run([sys.executable, str(GUARD)], input=json.dumps(payload),
                            capture_output=True, text=True, timeout=10)
    return result.returncode


ALLOWED = [
    'export PATH="/c/Program Files/SDCC/bin:$PATH" && pio run -d 868Receiver 2>&1 | tail -3',
    "py -3.12 VerisureDoorSensor/build_and_flash.py --compile-only",
    "py -3.12 MD_CAM/build_and_flash_md_cam.py --compile-only",
    '(cd VerisureDoorSensor && sdcc -mmcs51 --model-small -c main.c -o "$TMP/" 2>&1 | grep -c ": warning ")',
    "py -3.12 -m pytest tests -q; py -3.12 .github/scripts/check_secrets.py",
    "py -3.12 derive_key.py --master-key 000102030405060708090a0b0c0d0e0f --id 2K79A65A",
    "git status -s && git diff --stat && git -C 868Receiver log --oneline -3",
    "make -C cc1110_common/test_host",
    "git diff VerisureDoorSensor/build_and_flash.py",
    "grep -n counter_seed VerisureDoorSensor/build_and_flash.py",
    "rg build_and_flash_md_cam.py",
    "py -3.12 -m py_compile VerisureDoorSensor/build_and_flash.py",
    "py -3.12 -m pytest -k eval tests",
    "py -3.12 -m pytest -k exec tests",
    "grep eval README.md",
    "2>&1 grep counter_seed VerisureDoorSensor/build_and_flash.py",
]

BLOCKED = [
    "git commit -m x",
    "git status && git push origin HEAD",
    "git -C . switch main",
    "git checkout -- file",
    "/usr/bin/git.exe stash",
    "pio run -d 868Receiver -t upload",
    "pio run -d 868Receiver --target=upload",
    "py -3.12 VerisureDoorSensor/build_and_flash.py",
    "py -3.12 MD_CAM/build_and_flash_md_cam.py --port COM11",
    "py -3.12 868Receiver/ota_upload.py",
    "curl http://cc.local/flash",
    'bash -c "git push"',
    'py -c "import os; os.system(\'git push\')"',
    "python -m esptool write_flash 0 x.bin",
    'eval "git push"',
    'FOO=1 eval "git push"',
    "exec git push",
    'command eval "git push"',
    '(eval "git push")',
    '! eval "git push"',
    '{ eval "git push"; }',
    'if true; then eval "git push"; fi',
    'command -p eval "git push"',
    'command -- eval "git push"',
    'time -p eval "git push"',
    '</dev/null eval "git push"',
    '( ( eval "git push" ) )',
    'echo $(eval "git push")',
    'echo `eval "git push"`',
    'cat <(eval "git push")',
    "./VerisureDoorSensor/build_and_flash.py",
    "py -3.12 -m py_compile && py -3.12 VerisureDoorSensor/build_and_flash.py",
    'grep x $(py -3.12 VerisureDoorSensor/build_and_flash.py)',
    'cat <(py -3.12 VerisureDoorSensor/build_and_flash.py)',
    'git status $(py -3.12 VerisureDoorSensor/build_and_flash.py)',
    'py -3.12 -m py_compile $(py -3.12 VerisureDoorSensor/build_and_flash.py)',
    'rg --pre py x VerisureDoorSensor/build_and_flash.py',
    'git -c diff.external="py VerisureDoorSensor/build_and_flash.py" diff',
    'while eval "git push"; do break; done',
    'until eval "git push"; do break; done',
    'GIT_EXTERNAL_DIFF="py VerisureDoorSensor/build_and_flash.py" git diff',
    'GIT_EXTERNAL_DIFF=py git diff VerisureDoorSensor/build_and_flash.py',
    'git --exec-path=/tmp diff VerisureDoorSensor/build_and_flash.py',
    '2>&1 eval "git push"',
    "env -S 'git push'",
    "env --split-string='git push'",
    'command time -o /tmp/t eval "git push"',
    'env time -f %e eval "git push"',
]


@pytest.mark.parametrize("command", ALLOWED)
def test_qa_tester_allowed(command):
    assert run_guard(command) == 0


@pytest.mark.parametrize("command", BLOCKED)
def test_qa_tester_blocked(command):
    assert run_guard(command) == 2


@pytest.mark.parametrize("agent_type", [None, "embedded_coder", "code_reviewer"])
def test_other_agents_not_affected(agent_type):
    assert run_guard("git commit -m x && git push", agent_type) == 0


def test_bad_json_is_ignored():
    result = subprocess.run([sys.executable, str(GUARD)], input="not json",
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0
