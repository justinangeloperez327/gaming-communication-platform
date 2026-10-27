# Windows third-party source notices

The portable bundle now includes `third-party/README.md`, `catalog.json`, Qt
license texts, pinned Qt source attribution/copyright material and the LiveKit C++/Rust SDK, WebRTC and soxr reference
license/notice files at pinned upstream revisions. The catalog pins
the upstream commits, original UTF-8 bytes, sizes and SHA-256 hashes. Git
attributes disable line-ending conversion for these files so Windows checkout
does not invalidate upstream byte hashes. Package creation needs no network
access to obtain the notice files.

Packaging compares windeployqt's version with Qt 6.8.3 in the catalog and the
configured CMake SDK version with LiveKit C++ SDK 1.12.2. Updating either pin
requires updating and reviewing the baseline. This is not verification of the
identity of a manually supplied SDK or of every deployed DLL version.

The bundle verifier compares the copied catalog with the current source
revision and validates every baseline file independently of the general
payload manifest. Missing notices, empty/changed text or a modified coverage
statement fail verification, even if the payload manifest was regenerated.
Staged and extracted ZIP checks both apply this rule. Rejection fixtures use
synthetic executable payloads and the real baseline; they prove packaging
logic, not license completeness or runtime behavior.

## Work still required before binary distribution

- Match the exact deployed Qt modules, QML/plugins and bundled libraries to
  their attribution/source material and the intended licensing route.
- Inventory LiveKit FFI, WebRTC, codecs and other transitive runtime components
  from the exact pinned SDK build. The SDK's top-level NOTICE is not that list.
- Review any Microsoft runtime payload against its redistribution material.
- Record component versions, upstream sources, notices and review results with
  the binary inventory. Preserve applicable source/relinking material for the
  chosen distribution route rather than treating these texts as sufficient.

The packaged catalog deliberately says
`expanded_source_notices_build_review_pending`. CI passing means that the
declared baseline is present and intact; it does not close the attribution,
clean-PC, signing, physical media or gaming-performance acceptance gates.
No installer, service deployment or public release is created.

## Build-specific evidence added by Group 12

`DISTRIBUTION-INVENTORY.json` records every shipped EXE/DLL and the exact input
whose bytes it matches: a GCP build, the pinned LiveKit release ZIP, the selected
VS x64 CRT, or the installed Qt tree. Unknown inputs fail staging. The Qt binaries
are kept unpatched and relocated through `qt.conf`; this makes the comparison
meaningful. Matching an installed Qt tree does not authenticate that tree.

Staging preserves available Qt `sbom`/`licenses` text and SDK archive license/notice
files under `third-party/build`, with their own hashes and origins. Missing Qt
evidence remains visible as an empty evidence category, not an assertion of
complete coverage. The catalog also pins the C++ SDK's Rust submodule revision and
the exact release ZIP digest. The extra reference texts were checked against the
upstream Git blobs; a reference does not establish that every described component
is present in this binary build.

`source-catalog.json` maps the retained Qt source metadata and license/copyright
files to exact module revisions. Conditional/unused components can be present;
this is a source notice superset, not proof of deployed Qt build configuration.
The Rust source evidence includes the workspace lockfile, FFI manifest and
resampler build/header material at the SDK submodule revision. The workspace
lockfile includes build/example/platform-only crates and is not a Windows FFI SBOM.
In particular, the resampler build compiles libsoxr statically and its header
identifies LGPL v2.1; source/relinking review must cover it explicitly.

The declared coverage is `expanded_source_notices_build_review_pending`.
Reconcile exact module/build features, FFI Rust dependencies, WebRTC/codecs and
Microsoft material before marking attribution complete. Source delivery and the
chosen Qt licensing route also remain owner review items. See the
[Group 12 handoff](distribution-qualification.md) for the evidence boundaries.
