# Windows third-party notice baseline

The portable bundle now includes `third-party/README.md`, `catalog.json`, Qt
baseline license texts and the LiveKit SDK LICENSE/NOTICE. The catalog pins
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
`baseline_only_transitive_attributions_pending`. CI passing means that the
declared baseline is present and intact; it does not close the attribution,
clean-PC, signing, physical media or gaming-performance acceptance gates.
No installer, service deployment or public release is created.
