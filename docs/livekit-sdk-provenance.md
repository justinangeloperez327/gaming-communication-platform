# Group 57 — LiveKit dependency integrity on cached builds

The native LiveKit C++ SDK is pinned to version `1.12.2` and a Windows x64 release archive SHA-256 in `cmake/GcpLiveKit.cmake`. The download path already used `EXPECTED_HASH`, but a previously extracted SDK could skip the download-and-hash path.

The download-enabled CMake configuration now requires a matching cached archive and checks the SHA-256 even when the extracted CMake package already exists. Missing or modified archives fail configuration rather than silently trusting previously extracted SDK files.

**Boundary:** This validates the cached release archive, not each extracted file. A malicious local actor who can modify the extracted SDK directory without modifying the archive can still undermine the build. The optional user-supplied `GCP_LIVEKIT_SDK_PREFIX` remains a separately trusted input and is not authenticated by the pinned downloaded-archive checksum. Future qualification should verify SDK contents, dependency licenses and build provenance alongside final binary signing.

The change does not release, distribute, install or sign any artifact.
