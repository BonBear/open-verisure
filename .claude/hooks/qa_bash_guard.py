"""PreToolUse guard for Bash calls made by the qa_tester subagent.

AGENTS.md gives qa_tester execution rights only: it builds and tests, it never
commits, pushes, switches branches, flashes hardware or runs OTA updates. The
prompt in .claude/agents/qa_tester.md says so; this hook enforces it.

Claude Code passes the tool call as JSON on stdin. Hooks from settings files
also run inside subagents, and then the input carries "agent_type". Calls from
the main session or any other agent are not affected (exit 0). A blocked call
exits 2 and the reason on stderr is shown to the agent.

This is a guard rail against mistakes, not a sandbox: it inspects the command
text, so it errs on the side of blocking anything it cannot read clearly.
"""
import json
import re
import shlex
import sys

GUARDED_AGENT = "qa_tester"

# git subcommands qa_tester may run; everything else is blocked.
GIT_READ_ONLY = {"status", "diff", "log", "show", "grep", "ls-files",
                 "rev-parse", "describe", "blame"}
# git options that take a separate value before the subcommand.
GIT_OPTS_WITH_VALUE = {"-C", "-c", "--git-dir", "--work-tree", "--namespace"}

FLASH_SCRIPTS = re.compile(r"build_and_flash(_md_cam)?\.py")
ALWAYS_BLOCKED = [
    (re.compile(r"ota_upload"), "OTA updates"),
    (re.compile(r"esptool"), "esptool (flashing)"),
    (re.compile(r"\bcc(camera)?\.local\b"), "talking to the bench flashers"),
    (re.compile(r"(^|\s)(eval|exec)\s"), "eval/exec"),
    (re.compile(r"\b(ba|z|da)?sh\s+-c\b"), "nested shells"),
    (re.compile(r"\b(powershell|pwsh)(\.exe)?\b"), "nested shells"),
    (re.compile(r"\bcmd(\.exe)?\s+/c\b", re.I), "nested shells"),
    (re.compile(r"\b(python[0-9.]*|py)(\.exe)?\s+(-[0-9.]+\s+)?-c\b"), "inline python"),
]


def segments(cmd):
    """Split a shell command into simple commands on ; & | && || and newlines."""
    return [s.strip() for s in re.split(r"&&|\|\||[;&|\n]", cmd) if s.strip()]


def words(seg):
    try:
        return shlex.split(seg, posix=True)
    except ValueError:
        return seg.split()


def git_subcommand(ws):
    """Return the git subcommand in a word list starting at 'git', or ''."""
    i = 1
    while i < len(ws):
        w = ws[i]
        if w in GIT_OPTS_WITH_VALUE:
            i += 2
        elif w.startswith("-"):
            i += 1
        else:
            return w
    return ""


def check_segment(seg):
    for pattern, what in ALWAYS_BLOCKED:
        if pattern.search(seg):
            return what
    ws = words(seg)
    for idx, w in enumerate(ws):
        base = re.sub(r"(\.exe)?$", "", w.replace("\\", "/").rsplit("/", 1)[-1])
        if base == "git":
            sub = git_subcommand(ws[idx:])
            if sub not in GIT_READ_ONLY:
                return f"git {sub or '(no subcommand)'}; allowed: {', '.join(sorted(GIT_READ_ONLY))}"
        if base in ("pio", "platformio"):
            rest = ws[idx + 1:]
            if any(a in ("-t", "--target") or a.startswith("--target=")
                   or "upload" in a for a in rest):
                return "pio targets (upload); run a plain 'pio run -d <dir>'"
    if FLASH_SCRIPTS.search(seg) and "--compile-only" not in seg:
        return "build_and_flash scripts without --compile-only"
    return None


def main():
    try:
        data = json.load(sys.stdin)
    except ValueError:
        return 0
    if data.get("agent_type") != GUARDED_AGENT:
        return 0
    cmd = (data.get("tool_input") or {}).get("command", "")
    for seg in segments(cmd):
        why = check_segment(seg)
        if why:
            print(f"Blocked by .claude/hooks/qa_bash_guard.py: qa_tester may not "
                  f"run {why}. Command segment: {seg!r}. Report this to the "
                  f"Orchestrator instead.", file=sys.stderr)
            return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
