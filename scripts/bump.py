"""Prepare a qView release without committing, tagging, or pushing."""

import argparse
from datetime import date
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FILES = {
    "CMakeLists.txt": r"(?m)^project\(qView VERSION ([0-9]+\.[0-9]) LANGUAGES CXX\)(?=\r?$)",
    "dist/win/qView.iss": r'(?m)^#define MyAppVersion "([0-9]+\.[0-9])"(?=\r?$)',
}


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=["major", "minor"])
    args = parser.parse_args()

    branch = git("branch", "--show-current")
    if not branch or branch == "main" or git("status", "--porcelain", "--untracked-files=no"):
        parser.exit(1, "Start on a clean release branch, not main.\n")

    files = {}
    versions = set()
    for name, pattern in FILES.items():
        source = (ROOT / name).read_bytes().decode("utf-8")
        matches = list(re.finditer(pattern, source))
        if len(matches) != 1:
            parser.exit(1, f"Expected one version in {name}.\n")
        files[name] = (source, matches[0].span(1))
        versions.add(matches[0].group(1))
    if len(versions) != 1:
        parser.exit(1, "App and installer versions do not match.\n")

    old = versions.pop()
    major, minor = map(int, old.split("."))
    if args.kind == "minor" and minor >= 9:
        parser.exit(1, "qView treats version numbers as decimals; use a major bump or update version handling first.\n")
    new = f"{major + 1}.0" if args.kind == "major" else f"{major}.{minor + 1}"
    tag = f"v{new}"
    if git("tag", "--list", tag):
        parser.exit(1, f"Tag {tag} already exists locally.\n")

    changelog = ROOT / "CHANGELOG.md"
    notes = changelog.read_bytes().decode("utf-8")
    heading = list(re.finditer(r"(?m)^## \[Unreleased\](?=\r?$)", notes))
    if len(heading) != 1:
        parser.exit(1, "Expected one Unreleased heading in CHANGELOG.md.\n")
    if re.search(rf"(?m)^## \[{re.escape(new)}\]", notes):
        parser.exit(1, f"CHANGELOG.md already contains {new}.\n")
    newline = "\r\n" if "\r\n" in notes else "\n"
    pos = heading[0].end()
    if input(f"Bump {old} to {new}? [y/N] ").strip().lower() != "y":
        return

    for name, (source, (start, end)) in files.items():
        (ROOT / name).write_bytes((source[:start] + new + source[end:]).encode("utf-8"))
    notes = notes[:pos] + newline * 2 + f"## [{new}] - {date.today()}" + notes[pos:]
    changelog.write_bytes(notes.encode("utf-8"))
    print(f'''
Review and edit the release notes, then run:
  git add CMakeLists.txt dist/win/qView.iss CHANGELOG.md
  git commit -m "Prepare {tag} release"
  git switch main
  git pull --ff-only origin main
  git merge {shlex.quote(branch)}
  git tag -a {tag} -m "qView {tag}"
  git push --atomic origin main {tag}

Once the GitHub Release exists, run: make push
'''.strip())


if __name__ == "__main__":
    try:
        main()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit(str(exc)) from None
