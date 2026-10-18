"""Remove personal checkout/home paths from publishable verification evidence."""
from pathlib import Path


def redact(text: str, root: Path | None = None) -> str:
    root = root or Path(__file__).resolve().parents[1]
    for path, label in ((root.resolve(), "<repo>"), (Path.home(), "<home>")):
        # Programs can print either native or forward-slash paths on Windows.
        for spelling in sorted({str(path), path.as_posix()}, key=len, reverse=True):
            text = text.replace(spelling, label)
    return text
