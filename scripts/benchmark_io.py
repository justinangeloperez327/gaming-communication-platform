"""Bounded local benchmark inputs; file identity is not proof of physical origin."""
import hashlib
import json
from pathlib import Path

MAX_FILE_BYTES = 100 * 1024 * 1024


def checked_file(path, seen=None):
    try:
        path = Path(path).resolve(strict=True)
        info = path.stat()
    except OSError as error:
        raise ValueError("Capture file is unavailable") from error
    if not path.is_file() or not 0 < info.st_size <= MAX_FILE_BYTES:
        raise ValueError("Missing, empty or oversized benchmark input")
    identity = (info.st_dev, info.st_ino)
    if seen is not None:
        if path in seen or identity in seen:
            raise ValueError("Capture file reused through a path alias or hard link")
        seen.update((path, identity))
    return path


def distinct_files(paths):
    seen = set()
    return [checked_file(path, seen) for path in paths]


def no_duplicate_keys(items):
    result = {}
    for key, value in items:
        if key in result:
            raise ValueError("Duplicate JSON key")
        result[key] = value
    return result


def json_value(text):
    def bad_constant(_):
        raise ValueError("Nonfinite JSON number")
    return json.loads(text, object_pairs_hook=no_duplicate_keys, parse_constant=bad_constant)


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(65536), b""):
            value.update(chunk)
    return value.hexdigest()
