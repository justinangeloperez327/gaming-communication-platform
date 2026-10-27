"""Inspect PE import closure of a verified Windows bundle without loading code.

This is additional build-time evidence. It cannot prove every dynamically loaded
runtime, transitive static dependency, license obligation or consumer-PC behavior.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
POLICY = ROOT / "packaging/windows/pe-dependency-policy.json"
LIMITS = {"files": 4096, "report": 8 * 1024 * 1024, "binary": 256 * 1024 * 1024}


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def safe_relative(value: str) -> str:
    if not isinstance(value, str) or not value or len(value) > 240 or "\\" in value:
        raise ValueError("Unsafe relative path")
    parts = value.split("/")
    if any(not p or p in {".", ".."} or p[-1:] in {".", " "} or
           not re.fullmatch(r"[A-Za-z0-9_. +@()-]+", p) or
           p.split(".")[0].upper() in {"CON", "PRN", "AUX", "NUL", *[f"COM{i}" for i in range(1, 10)],
                                       *[f"LPT{i}" for i in range(1, 10)]} for p in parts):
        raise ValueError("Unsafe relative path")
    return value


def package_files(root: Path) -> dict[str, Path]:
    if root.is_symlink() or getattr(root, "is_junction", lambda: False)():
        raise ValueError("Package root cannot be a link")
    files = {}
    for path in root.rglob("*"):
        if path.is_symlink() or getattr(path, "is_junction", lambda: False)():
            raise ValueError("Package links are not permitted")
        if path.is_file():
            name = safe_relative(path.relative_to(root).as_posix())
            if name.casefold() in files:
                raise ValueError("Case-colliding package paths")
            files[name.casefold()] = path
    if len(files) > LIMITS["files"]:
        raise ValueError("Too many package files")
    return files


def pe_imports(path: Path) -> tuple[list[str], list[str]]:
    """Read PE32+ import and delay-import names without loading executable code."""
    if path.stat().st_size > LIMITS["binary"]:
        raise ValueError("PE file exceeds bound")
    data = path.read_bytes()

    def unpack(fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(data):
            raise ValueError("Truncated PE structure")
        return struct.unpack_from(fmt, data, offset)

    if data[:2] != b"MZ":
        raise ValueError("Not a PE executable")
    pe, = unpack("<I", 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("Invalid PE signature")
    machine, count, _, _, _, optional_size, _ = unpack("<HHIIIHH", pe + 4)
    optional = pe + 24
    if machine != 0x8664 or not 1 <= count <= 96 or optional_size < 112:
        raise ValueError("Expected bounded Windows x64 PE")
    if unpack("<H", optional)[0] != 0x20B:
        raise ValueError("Expected PE32+")
    directories, = unpack("<I", optional + 108)
    if directories > 16 or 112 + directories * 8 > optional_size:
        raise ValueError("Invalid PE data directories")
    sections = []
    for index in range(count):
        offset = optional + optional_size + index * 40
        size, address, raw_size, raw = unpack("<IIII", offset + 8)
        if raw + raw_size > len(data):
            raise ValueError("Truncated PE section")
        sections.append((address, max(size, raw_size), raw, raw_size))

    def file_offset(rva, size=1):
        for address, virtual_size, raw, raw_size in sections:
            delta = rva - address
            if 0 <= delta < virtual_size and delta + size <= raw_size:
                return raw + delta
        raise ValueError("PE RVA outside file-backed sections")

    def name_at(rva):
        offset = file_offset(rva)
        end = data.find(b"\0", offset, min(offset + 261, len(data)))
        if end < 0:
            raise ValueError("Unterminated PE dependency")
        name = data[offset:end].decode("ascii").lower()
        if not re.fullmatch(r"[a-z0-9_.-]+\.dll", name):
            raise ValueError("Unsafe PE import name")
        return name

    def imports(index, stride, delayed):
        if directories <= index:
            return []
        rva, size = unpack("<II", optional + 112 + index * 8)
        if rva == size == 0:
            return []
        if not rva or size < stride or size > 1024 * 1024:
            raise ValueError("Invalid PE import directory")
        result = []
        for offset in range(0, size - stride + 1, stride):
            row = unpack("<" + "I" * (stride // 4), file_offset(rva + offset, stride))
            if not any(row):
                return sorted(set(result))
            if len(result) >= 1024:
                raise ValueError("Too many PE imports")
            if delayed and row[0] != 1:
                raise ValueError("Unsupported non-RVA delay import")
            result.append(name_at(row[1] if delayed else row[3]))
        raise ValueError("Unterminated PE import directory")

    return imports(1, 20, False), imports(13, 32, True)


def resolve_imports(path: str, names: list[str], files: dict[str, Path], policy: dict) -> list[dict]:
    result = []
    folder = PurePosixPath(path).parent
    for name in names:
        candidates = [(folder / name).as_posix().casefold(), name]
        target = next((files[n].relative_to(files[n].parents[len(PurePosixPath(n).parts) - 1]).as_posix()
                       for n in candidates if n in files), None)
        # Preserve canonical package casing, without retaining local build paths.
        if target:
            result.append({"name": name, "kind": "bundled", "path": target})
        elif name in policy["windows_system_dlls"] or re.fullmatch(r"(?:api|ext)-ms-win-[a-z0-9-]+\.dll", name):
            result.append({"name": name, "kind": "windows_system"})
        else:
            raise ValueError(f"Unresolved dependency {name} in {path}")
    return result


def analyze(bundle: Path, commit: str) -> dict:
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("Expected full source commit")
    root = bundle.resolve()
    files = package_files(root)
    inventory_path = root / "DISTRIBUTION-INVENTORY.json"
    if inventory_path.stat().st_size > LIMITS["report"]:
        raise ValueError("Source inventory exceeds bound")
    inventory = json.loads(inventory_path.read_text(encoding="utf-8-sig"))
    if inventory.get("commit_sha") != commit or inventory.get("artifact_kind") != "windows_distribution_build_inventory":
        raise ValueError("Source inventory identity mismatch")
    for key in ("attribution_complete", "production_qualified", "physical_media_verified", "clean_consumer_windows_verified"):
        if inventory.get(key) is not False:
            raise ValueError("Unsupported source inventory qualification claim")
    expected = {}
    for row in inventory["executables"]:
        key = safe_relative(row["path"]).casefold()
        if key in expected:
            raise ValueError("Duplicate source executable")
        expected[key] = row
    binaries = {key: path for key, path in files.items() if path.suffix.lower() in {".exe", ".dll"}}
    if binaries.keys() != expected.keys() or not 5 <= len(binaries) <= LIMITS["files"]:
        raise ValueError("Source inventory does not cover the executable payload")
    policy = json.loads(POLICY.read_text())
    rows = []
    for key, path in sorted(binaries.items()):
        if expected[key]["sha256"] != digest(path) or expected[key]["bytes"] != path.stat().st_size:
            raise ValueError("Source executable digest mismatch")
        name = path.relative_to(root).as_posix()
        regular, delayed = pe_imports(path)
        rows.append({"path": name, "sha256": digest(path),
                     "imports": resolve_imports(name, regular, files, policy),
                     "delay_imports": resolve_imports(name, delayed, files, policy)})
    return {"schema_version": 1, "artifact_kind": "windows_pe_import_closure",
            "commit_sha": commit, "distribution_inventory_sha256": digest(inventory_path),
            "policy_sha256": digest(POLICY), "executable_count": len(rows), "executables": rows,
            "declared_imports_resolved": True, "all_dynamic_loading_verified": False,
            "attribution_complete": False, "consumer_pc_verified": False, "production_qualified": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = analyze(args.bundle, args.commit)
    if args.output.resolve().is_relative_to(args.bundle.resolve()):
        raise ValueError("Import evidence must remain outside the verified payload")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print(f"Inspected regular/delayed imports of {report['executable_count']} x64 executables; consumer-PC and attribution gates remain open.")


