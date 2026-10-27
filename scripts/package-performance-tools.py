#!/usr/bin/env python3
"""Create a source-only operator toolkit; excludes captures, credentials and fixtures."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import zipfile

FILES = ("scripts/desktop-performance.psm1", "scripts/capture-desktop-performance.ps1",
         "scripts/benchmark_io.py", "scripts/desktop_performance.py",
         "scripts/compare-frame-times.py", "scripts/qualify-desktop-performance.py",
         "docs/desktop-gaming-qualification.md", "docs/examples/desktop-performance-manifest.json")


def package(root, output, commit):
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("Expected source commit SHA")
    root, output = Path(root).resolve(), Path(output).resolve()
    contents, inventory = {}, []
    for name in FILES:
        path = root/name
        if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size < 1024*1024:
            raise ValueError("Required toolkit source missing, symlinked or oversized")
        data = path.read_bytes()
        contents[name] = data
        inventory.append({"path":name,"bytes":len(data),"sha256":hashlib.sha256(data).hexdigest()})
    contents["tool-manifest.json"] = (json.dumps({"schema":1,"source_commit":commit,"files":inventory,
        "physical_evidence_included":False,"qualification_implied":False},indent=2)+"\n").encode()
    contents["README.md"] = ("# GCP gaming measurement tools\n\n"
        "Read docs/desktop-gaming-qualification.md before capture. Windows PowerShell 5.1+ collects process/counter data; Python 3.12+ analyzes it.\n\n"
        "These source tools do not include the app, PresentMon, any game, captures or physical qualification. Source commit: `"+commit+"`.\n").encode()
    output.parent.mkdir(parents=True,exist_ok=True)
    checksum = output.with_suffix(".sha256")
    if checksum.exists():
        raise ValueError("Checksum already exists; choose a new output path")
    with output.open("xb") as destination:
        with zipfile.ZipFile(destination,"w",compression=zipfile.ZIP_DEFLATED) as archive:
            for name in sorted(contents):
                info=zipfile.ZipInfo(name,date_time=(1980,1,1,0,0,0));info.compress_type=zipfile.ZIP_DEFLATED
                info.external_attr=0o100644 << 16
                archive.writestr(info,contents[name])
    value=hashlib.sha256(output.read_bytes()).hexdigest()
    with checksum.open("x",encoding="ascii") as target:target.write(value+"\n")
    return value


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output",type=Path,required=True);parser.add_argument("--commit",required=True)
    args=parser.parse_args(argv)
    try:
        package(Path(__file__).resolve().parent.parent,args.output,args.commit)
        print("Packaged source-only measurement tools; no captures or physical qualification included.")
        return 0
    except (OSError,ValueError) as error:
        print(f"Toolkit packaging failed: {error}",file=sys.stderr);return 1


if __name__=="__main__":sys.exit(main())
