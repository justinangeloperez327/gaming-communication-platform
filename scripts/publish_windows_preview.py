"""Publish the explicitly configured alpha tag once; never replace a public release."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def gh(*args):
    return subprocess.run(["gh", *args], check=True, capture_output=True, text=True).stdout


def publish(artifact_dir):
    repository = os.environ["GITHUB_REPOSITORY"]
    commit = os.environ["GITHUB_SHA"]
    if repository != "justinangeloperez327/gaming-communication-platform" or not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("Unexpected release repository or commit")
    config = json.loads((ROOT / "packaging/windows/version.json").read_text())
    version, tag = config["version"], config["tag"]
    if not re.fullmatch(r"\d+\.\d+\.\d+", version) or tag != f"v{version}-alpha.1":
        raise ValueError("Invalid alpha version")
    try:
        existing = json.loads(gh("api", f"repos/{repository}/releases/tags/{tag}"))
    except subprocess.CalledProcessError as error:
        if "HTTP 404" not in error.stderr:
            raise
        existing = None
    if existing and not existing["draft"]:
        print(f"Preview {tag} already published; leaving its assets and tag unchanged.")
        return
    if existing and existing["target_commitish"] != commit:
        raise ValueError("Existing draft belongs to a different commit")

    label = tag[1:]
    inputs = [
        (artifact_dir / "portable/gcp-private-windows-x64.zip", artifact_dir / "portable/gcp-private-windows-x64.sha256", f"GCP-{label}-windows-x64-portable.zip"),
        (artifact_dir / f"installer/GCP-{label}-windows-x64-setup.exe", artifact_dir / f"installer/GCP-{label}-windows-x64-setup.exe.sha256", f"GCP-{label}-windows-x64-setup.exe"),
    ]
    output = artifact_dir / "publish"
    output.mkdir(exist_ok=True)
    assets, sums = [], []
    for source, checksum_source, name in inputs:
        with source.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        if checksum_source.read_text().strip() != digest:
            raise ValueError(f"Artifact checksum mismatch: {source.name}")
        destination = output / name
        shutil.copyfile(source, destination)
        checksum = output / (name + ".sha256")
        checksum.write_text(digest + "\n")
        assets.extend([destination, checksum])
        sums.append(f"{digest}  {name}")
    checksums = output / "SHA256SUMS.txt"
    checksums.write_text("\n".join(sums) + "\n")
    run_url = f"https://github.com/{repository}/actions/runs/{os.environ['GITHUB_RUN_ID']}"
    provenance = output / "BUILD.json"
    provenance.write_text(json.dumps({"commit": commit, "ci_run": run_url, "tag": tag, "unsigned": True}, indent=2) + "\n")
    assets.extend([checksums, provenance])
    notes = output / "release-notes.md"
    notes.write_text((ROOT / "packaging/windows/release-notes.md").read_text() + f"\nSource: `{commit}`\n\nCI: {run_url}\n")
    if existing is None:
        gh("release", "create", tag, "--repo", repository, "--target", commit, "--draft", "--prerelease",
           "--title", f"GCP {label} — unsigned Windows preview", "--notes-file", str(notes))
    gh("release", "upload", tag, "--repo", repository, *map(str, assets), "--clobber")
    gh("release", "edit", tag, "--repo", repository, "--draft=false", "--prerelease")
    print(f"Published {tag} from {commit} with verified portable ZIP and installer.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact-dir", type=Path, required=True)
    publish(parser.parse_args().artifact_dir)
