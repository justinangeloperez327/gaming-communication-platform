# Third-party source notices and evidence

This private test bundle uses Qt 6.8.3 (including Qt WebSockets for desktop
text events) and the LiveKit C++ SDK 1.12.2.
The accompanying catalog records exact upstream commits, source URLs, file
sizes and SHA-256 hashes for the included materials. They are upstream
texts, not a license granted to GCP by this document. Including alternative
Qt license texts does not select a license or establish redistribution rights.

Coverage is **expanded source notices; build-specific attribution remains pending**.
`source-catalog.json` identifies the Qt components and their original metadata
and license/copyright files, including conditional components that may not be
enabled in the deployed kit. The Qt source set is a conservative superset, not
a resolved SBOM for these binaries. Original upstream bytes are preserved.

`livekit-rust` retains the exact FFI source revision's license/notice, lockfile,
WebRTC notices and resampler build/header evidence. The libsoxr source identifies
LGPL v2.1 and its build script compiles a static library. Appropriate corresponding
source/relinking material and license selection still need build-specific review.
The workspace lockfile also includes build/example/platform-only packages; it
does not establish which Rust crates are present in the Windows FFI binary.

The LiveKit NOTICE identifies LiveKit itself; it is not a complete notice set
for livekit_ffi.dll, WebRTC, codecs, Rust dependencies or the SDK's other
transitive components. Qt's plugins/QML modules and their bundled third-party
components also require a build-specific attribution inventory. Microsoft
runtime redistribution requirements must be checked for the actual payload.

Before sharing binaries, reconcile the exact deployed DLL/QML inventory with
upstream attribution material, the chosen Qt licensing/source-delivery route,
and applicable component notices. Keep that review with the exact build.
`DISTRIBUTION-INVENTORY.json` at the bundle root records the executable inventory
and source-kit/archive matching. CI separately inspects regular and delayed PE
imports, without treating them as evidence of every dynamically loaded runtime. It deliberately leaves attribution,
signing and physical qualification gates open. Windows system DLLs are identified
as external operating-system dependencies and are not copied by this inventory.
The Microsoft runtime's distribution terms depend on the owner's licensed tools.

These materials are not a compliance certificate or an approval to distribute.
See docs/windows-third-party-notices.md in the source repository for the
remaining packaging work. The alpha installer uses this same notice set.

Staging also preserves available upstream Qt SBOM/license and pinned SDK archive
notice files under `third-party/build`, with hashes in the build inventory. These
do not authenticate a manually supplied Qt kit or close attribution/source delivery.
