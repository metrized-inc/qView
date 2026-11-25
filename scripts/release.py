"""Print the changelog entry for an exact qView release tag."""

import argparse
from pathlib import Path
import re

from bump import FILES, ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r"v[0-9]+\.[0-9]", args.tag):
        parser.error("expected a vMAJOR.MINOR tag with a single-digit minor version")
    version = args.tag[1:]
    for name, pattern in FILES.items():
        if re.findall(pattern, (ROOT / name).read_text(encoding="utf-8")) != [version]:
            parser.error(f"{name} does not match {args.tag}")
    changelog = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    headings = list(re.finditer(r"(?m)^## (.+)$", changelog))
    for i, heading in enumerate(headings):
        if re.fullmatch(rf"\[{re.escape(version)}\] - \d{{4}}-\d{{2}}-\d{{2}}", heading[1]):
            end = headings[i + 1].start() if i + 1 < len(headings) else len(changelog)
            notes = changelog[heading.end():end].strip()
            if not notes:
                parser.error(f"empty changelog entry for {args.tag}")
            if args.output:
                args.output.write_text(notes + "\n", encoding="utf-8")
            else:
                print(notes)
            return
    parser.error(f"no changelog entry for {args.tag}")


if __name__ == "__main__":
    main()
