#!/usr/bin/env python3
"""Build the release package: a zip with only what is needed to set the system up.

    python release/make_release.py            # dist/open-verisure-<version>.zip from HEAD
    python release/make_release.py --check    # also unpack it and build all three firmwares

The package holds the gateway firmware sources, both CC1110 firmwares with their build and
flash scripts, the key and flash tools, the Blackfin program and JTAG scripts for converting a
camera, and the user documentation. It leaves out the research notes and logs (MD_CAM/docs,
VerisureDoorSensor/docs, SIREN), the agent files and TODO.

Only files tracked by git are packed (git archive), so secrets.h and config.h, which are
generated on the user's machine and git-ignored, can never end up in it. Nothing is prebuilt:
every CC1110 firmware carries its own device key and counter seed, and the gateway's key is set
on its setup page or in secrets.h.
"""
import argparse, os, re, shutil, subprocess, sys, tempfile, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# paths (files or directories) in the package; a missing one is skipped with a note
INCLUDE = [
    "README.md", "LICENSE", "SECURITY.md", "requirements.txt",
    "derive_key.py", "cc1110_tools.py", "port_selector.py", "monitor.py",
    "868Receiver/README.md", "868Receiver/platformio.ini", "868Receiver/.gitignore", "868Receiver/src",
    "cc1110_common",
    "VerisureDoorSensor/README.md", "VerisureDoorSensor/main.c", "VerisureDoorSensor/config.h.example",
    "VerisureDoorSensor/build_and_flash.py",
    "MD_CAM/README.md", "MD_CAM/build_and_flash_md_cam.py", "MD_CAM/firmware", "MD_CAM/blackfin",
    "MD_CAM/test_camera_e2e.py", "MD_CAM/test_resend.py", "MD_CAM/dump_cc1110.py",
    "documentation",
]
# never in the package, even if tracked by mistake
FORBIDDEN = re.compile(r"(^|/)(secrets\.h|config\.h)$")


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, check=True, capture_output=True, text=True).stdout


def build(ref, out_dir):
    version = git("describe", "--tags", "--always", ref).strip()
    tracked = set(git("ls-tree", "-r", "--name-only", ref).splitlines())
    paths = []
    for p in INCLUDE:
        if p in tracked or any(t.startswith(p + "/") for t in tracked):
            paths.append(p)
        else:
            print(f"[!] not in {ref}, skipped: {p}")
    files = sorted(t for t in tracked if any(t == p or t.startswith(p + "/") for p in paths))
    bad = [f for f in files if FORBIDDEN.search(f)]
    if bad:
        sys.exit(f"[-] refusing to pack generated config files: {bad}")
    os.makedirs(out_dir, exist_ok=True)
    name = f"open-verisure-{version}"
    zip_path = os.path.join(out_dir, name + ".zip")
    tmp = tempfile.mkdtemp(prefix="ov-pack-")
    try:
        tar = os.path.join(tmp, "pack.tar")
        subprocess.run(["git", "archive", "--format=tar", f"--prefix={name}/", "-o", tar, ref, *paths],
                       cwd=ROOT, check=True)
        shutil.unpack_archive(tar, tmp)
        os.remove(tar)
        n = relink(os.path.join(tmp, name), github_blob(ref))
        if n:
            print(f"[+] {n} links to files outside the package now point at GitHub")
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
            for d, _, fs in os.walk(tmp):
                for f in sorted(fs):
                    p = os.path.join(d, f)
                    z.write(p, os.path.relpath(p, tmp).replace(os.sep, "/"))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"[+] {zip_path}: {len(files)} files")
    return zip_path, name


def github_blob(ref):
    """https://github.com/<owner>/<repo>/blob/<commit>/ for the origin remote, or None."""
    try:
        url = git("remote", "get-url", "origin").strip()
    except subprocess.CalledProcessError:
        return None
    m = re.match(r"(?:https://github\.com/|git@github\.com:)([^/]+/[^/]+?)(?:\.git)?$", url)
    if not m:
        return None
    sha = git("rev-parse", ref).strip()
    if not git("branch", "-r", "--contains", sha).strip():
        print(f"[!] {sha[:7]} is not on any remote branch yet: the GitHub links in the package "
              "give 404 until it is pushed")
    return f"https://github.com/{m.group(1)}/blob/{sha}/"


def relink(top, blob):
    """Point relative Markdown links to files left out of the package at the same commit on
    GitHub, so the package has no dead links. Returns the number of links changed."""
    if not blob:
        return 0
    count = 0
    for d, _, fs in os.walk(top):
        for f in fs:
            if not f.endswith(".md"):
                continue
            p = os.path.join(d, f)
            s = open(p, encoding="utf-8", newline="").read()

            def fix(m):
                nonlocal count
                link, anchor = m.group(2), m.group(3) or ""
                if re.match(r"[a-z]+:", link) or os.path.exists(os.path.normpath(os.path.join(d, link))):
                    return m.group(0)
                rel = os.path.relpath(os.path.normpath(os.path.join(d, link)), top).replace(os.sep, "/")
                if rel.startswith(".."):
                    return m.group(0)
                count += 1
                return f"{m.group(1)}{blob}{rel}{anchor})"

            s2 = re.sub(r"(\]\()([^)#\s]+)(#[^)]*)?\)", fix, s)
            if s2 != s:
                open(p, "w", encoding="utf-8", newline="").write(s2)
    return count


def broken_links(top):
    """Relative Markdown links in the package that point at a file it does not contain."""
    out = []
    for d, _, fs in os.walk(top):
        for f in fs:
            if not f.endswith(".md"):
                continue
            p = os.path.join(d, f)
            for m in re.finditer(r"\]\(([^)#\s]+)(#[^)]*)?\)", open(p, encoding="utf-8").read()):
                link = m.group(1)
                if re.match(r"[a-z]+:", link):
                    continue
                if not os.path.exists(os.path.normpath(os.path.join(d, link))):
                    out.append(f"{os.path.relpath(p, top)} -> {link}")
    return out


def check(zip_path, name):
    tmp = tempfile.mkdtemp(prefix="ov-release-")
    try:
        zipfile.ZipFile(zip_path).extractall(tmp)
        top = os.path.join(tmp, name)
        links = broken_links(top)
        print(f"[{'!' if links else '+'}] links to files outside the package: {len(links)}")
        for l in links:
            print("    " + l)
        ok = True
        shutil.copy(os.path.join(top, "868Receiver/src/secrets.h.example"), os.path.join(top, "868Receiver/src/secrets.h"))
        for d in ("VerisureDoorSensor", "MD_CAM/firmware"):
            shutil.copy(os.path.join(top, d, "config.h.example"), os.path.join(top, d, "config.h"))
        steps = [
            ("door sensor", [sys.executable, "VerisureDoorSensor/build_and_flash.py", "--compile-only"]),
            ("camera", [sys.executable, "MD_CAM/build_and_flash_md_cam.py", "--compile-only"]),
            ("gateway", ["pio", "run", "-d", "868Receiver"]),
        ]
        for what, cmd in steps:
            if cmd[0] == "pio" and not shutil.which("pio"):
                print(f"[!] {what}: pio not on PATH, skipped")
                continue
            r = subprocess.run(cmd, cwd=top, capture_output=True, text=True)
            print(f"[{'+' if r.returncode == 0 else '-'}] {what} build from the package")
            if r.returncode:
                ok = False
                print((r.stdout + r.stderr)[-2000:])
        return ok
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--ref", default="HEAD", help="git ref to pack (default HEAD; uncommitted changes are not packed)")
    ap.add_argument("--out", default=os.path.join(ROOT, "dist"), help="output directory (default dist/)")
    ap.add_argument("--check", action="store_true", help="unpack and build the firmwares from the package")
    a = ap.parse_args()
    zip_path, name = build(a.ref, a.out)
    if a.check and not check(zip_path, name):
        sys.exit(1)


if __name__ == "__main__":
    main()
