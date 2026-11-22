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
CMD_SUBST = re.compile(r"\$\(|`|[<>]\(")
VAR_ASSIGN = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=")
REDIR_PREFIX = re.compile(r"^(?:[0-9]*[<>]+|&>+)")
REDIR_OP_ONLY = re.compile(r"^(?:[0-9]*[<>]+|&>+)$")
SHELL_KEYWORDS = {"!", "{", "if", "elif", "then", "else", "do", "while", "until", "coproc"}
CMD_WRAPPERS = {"env", "command", "builtin", "nohup", "time"}
WRAPPER_OPTS_WITH_VALUE = {"-u", "-C", "--chdir", "--unset", "-o", "--output", "-f", "--format"}
DANGEROUS_FLASH_ENV = (
    "GIT_EXTERNAL_DIFF=",
    "GIT_EXEC_PATH=",
    "GIT_CONFIG_PARAMETERS=",
    "GIT_CONFIG_COUNT=",
    "RIPGREP_CONFIG_PATH=",
)
READ_ONLY_INSPECT = {
    "grep", "egrep", "fgrep", "rg", "head", "tail", "cat", "wc", "ls", "stat",
    "file", "diff",
}
PYTHON_CMD = re.compile(r"^(python[0-9.]*|py)$")
PY_VER_FLAG = re.compile(r"^-[0-9.]+(-[0-9]+)?$")
ALWAYS_BLOCKED = [
    (re.compile(r"ota_upload"), "OTA updates"),
    (re.compile(r"esptool"), "esptool (flashing)"),
    (re.compile(r"\bcc(camera)?\.local\b"), "talking to the bench flashers"),
    (re.compile(r"\b(ba|z|da)?sh\s+-c\b"), "nested shells"),
    (re.compile(r"\b(powershell|pwsh)(\.exe)?\b"), "nested shells"),
    (re.compile(r"\bcmd(\.exe)?\s+/c\b", re.I), "nested shells"),
    (re.compile(r"\b(python[0-9.]*|py)(\.exe)?\s+(-[0-9.]+\s+)?-c\b"), "inline python"),
]


def segments(cmd):
    """Split a shell command into simple commands on ; & | && || and newlines."""
    return [s.strip() for s in re.split(r"&&|\|\||(?<![<>])&(?![>])|[;|\n]", cmd) if s.strip()]


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


def cmd_base(w):
    """Return the basename of w with any trailing .exe stripped."""
    return re.sub(r"(\.exe)?$", "", w.replace("\\", "/").rsplit("/", 1)[-1], flags=re.I)


def is_env_split_opt(flag):
    """Return True if flag is env's -S or --split-string option."""
    return (
        flag == "--split-string"
        or flag.startswith("--split-string=")
        or bool(re.match(r"^-[i0v]*S", flag))
    )


def _scan_command_prefix(ws):
    """Return (cmd_idx, saw_env_split) after prefixes, redirections, assignments and wrappers."""
    i = 0
    saw_env_split = False
    while i < len(ws):
        w = ws[i]
        if w.startswith("("):
            stripped = w.lstrip("(")
            if not stripped:
                i += 1
                continue
            ws[i] = stripped
            w = stripped
        if w in SHELL_KEYWORDS or VAR_ASSIGN.match(w):
            i += 1
        elif REDIR_OP_ONLY.match(w):
            i += 2
        elif REDIR_PREFIX.match(w):
            i += 1
        elif cmd_base(w) in CMD_WRAPPERS:
            is_env = cmd_base(w) == "env"
            i += 1
            while i < len(ws):
                flag = ws[i]
                if flag == "--":
                    i += 1
                    break
                if is_env and is_env_split_opt(flag):
                    saw_env_split = True
                if flag in WRAPPER_OPTS_WITH_VALUE:
                    i += 2
                elif flag.startswith("-") or VAR_ASSIGN.match(flag):
                    i += 1
                else:
                    break
        else:
            break
    return (i if i < len(ws) else None), saw_env_split


def command_index(ws):
    """Return index of the command word in ws after prefixes, redirections, assignments and wrappers."""
    return _scan_command_prefix(ws)[0]


def is_py_compile(cmd_ws):
    """Return True if cmd_ws invokes python/py with -m py_compile."""
    if not cmd_ws or not PYTHON_CMD.match(cmd_base(cmd_ws[0])):
        return False
    i = 1
    if i < len(cmd_ws) and PY_VER_FLAG.match(cmd_ws[i]):
        i += 1
    return i + 1 < len(cmd_ws) and cmd_ws[i] == "-m" and cmd_ws[i + 1] == "py_compile"


def has_git_config_opt(git_ws):
    """Return True if git_ws passes -c, --config-env, or --exec-path before the subcommand."""
    i = 1
    while i < len(git_ws):
        w = git_ws[i]
        if (w.startswith("-c") or w.startswith("--config-env")
                or w == "--exec-path" or w.startswith("--exec-path=")):
            return True
        if w in GIT_OPTS_WITH_VALUE:
            i += 2
        elif w.startswith("-"):
            i += 1
        else:
            return False
    return False


def has_dangerous_rg_flag(args):
    """Return True if rg args include --pre or --hostname-bin."""
    return any(
        a in ("--pre", "--hostname-bin") or a.startswith(("--pre=", "--hostname-bin="))
        for a in args
    )


def check_segment(seg):
    for pattern, what in ALWAYS_BLOCKED:
        if pattern.search(seg):
            return what
    has_cmd_subst = bool(CMD_SUBST.search(seg))
    if has_cmd_subst and re.search(r"\b(eval|exec)\b", seg):
        return "eval/exec"
    ws = words(seg)
    cmd_idx, saw_env_split = _scan_command_prefix(ws)
    if saw_env_split:
        return "env -S (command splitting)"
    cmd = cmd_base(ws[cmd_idx]) if cmd_idx is not None else ""
    if cmd in ("eval", "exec"):
        return "eval/exec"
    for idx, w in enumerate(ws):
        base = cmd_base(w)
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
        prefix = ws[:cmd_idx] if cmd_idx is not None else ws
        dangerous_prefix = any(
            FLASH_SCRIPTS.search(t) or t.startswith(DANGEROUS_FLASH_ENV)
            for t in prefix
        )
        if has_cmd_subst or dangerous_prefix:
            allowed_flash_read = False
        elif cmd == "git":
            allowed_flash_read = not has_git_config_opt(ws[cmd_idx:])
        elif cmd == "rg":
            allowed_flash_read = not has_dangerous_rg_flag(ws[cmd_idx + 1:])
        elif cmd in READ_ONLY_INSPECT:
            allowed_flash_read = True
        elif cmd_idx is not None and is_py_compile(ws[cmd_idx:]):
            allowed_flash_read = True
        else:
            allowed_flash_read = False
        if not allowed_flash_read:
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
