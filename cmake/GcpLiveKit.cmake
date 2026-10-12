# The optional native LiveKit integration uses a fixed, checksum-verified release.
# Neither the standard desktop build nor the normal Rust build needs this SDK.
set(GCP_LIVEKIT_VERSION "1.12.2")
set(GCP_LIVEKIT_WINDOWS_X64_SHA256
    "196f1e86b306c427baed71eeea8c1ea1f8f40a10f087e646447719e3c647bdbb")

set(GCP_LIVEKIT_SDK_PREFIX "" CACHE PATH
    "Path to a locally installed LiveKit C++ SDK (optional)")
option(GCP_LIVEKIT_DOWNLOAD "Download pinned SDK for opt-in LiveKit builds" OFF)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "LiveKit smoke test currently supports only Windows x64")
endif()

if(GCP_LIVEKIT_SDK_PREFIX)
    if(NOT EXISTS "${GCP_LIVEKIT_SDK_PREFIX}/lib/cmake/LiveKit")
        message(FATAL_ERROR
            "LiveKit package not found in GCP_LIVEKIT_SDK_PREFIX: ${GCP_LIVEKIT_SDK_PREFIX}")
    endif()
    set(_gcp_sdk_prefix "${GCP_LIVEKIT_SDK_PREFIX}")
elseif(GCP_LIVEKIT_DOWNLOAD)
    set(_gcp_asset "livekit-sdk-windows-x64-${GCP_LIVEKIT_VERSION}.zip")
    set(_gcp_download_dir "${CMAKE_BINARY_DIR}/_downloads")
    set(_gcp_extract_dir "${CMAKE_BINARY_DIR}/_deps/livekit-sdk")
    set(_gcp_sdk_prefix
        "${_gcp_extract_dir}/livekit-sdk-windows-x64-${GCP_LIVEKIT_VERSION}")
    file(MAKE_DIRECTORY "${_gcp_download_dir}" "${_gcp_extract_dir}")
    set(_gcp_archive "${_gcp_download_dir}/${_gcp_asset}")
    if(NOT EXISTS "${_gcp_sdk_prefix}/lib/cmake/LiveKit")
        # EXPECTED_HASH also verifies the downloaded file before extraction.
        file(DOWNLOAD
            "https://github.com/livekit/client-sdk-cpp/releases/download/v${GCP_LIVEKIT_VERSION}/${_gcp_asset}"
            "${_gcp_archive}"
            EXPECTED_HASH "SHA256=${GCP_LIVEKIT_WINDOWS_X64_SHA256}"
            TLS_VERIFY ON
            STATUS _gcp_download_status
        )
        list(GET _gcp_download_status 0 _gcp_download_code)
        if(NOT _gcp_download_code EQUAL 0)
            message(FATAL_ERROR
                "LiveKit SDK download failed (${_gcp_download_status}). "
                "Use GCP_LIVEKIT_SDK_PREFIX for an offline install.")
        endif()
        file(ARCHIVE_EXTRACT INPUT "${_gcp_archive}" DESTINATION "${_gcp_extract_dir}")
    endif()
    if(NOT EXISTS "${_gcp_sdk_prefix}/lib/cmake/LiveKit")
        message(FATAL_ERROR "Downloaded LiveKit archive does not contain the expected CMake package")
    endif()
else()
    message(FATAL_ERROR
        "GCP_ENABLE_LIVEKIT requires GCP_LIVEKIT_SDK_PREFIX or GCP_LIVEKIT_DOWNLOAD=ON")
endif()

list(PREPEND CMAKE_PREFIX_PATH "${_gcp_sdk_prefix}")
find_package(LiveKit CONFIG REQUIRED)

if(NOT TARGET LiveKit::livekit)
    message(FATAL_ERROR "LiveKit C++ SDK did not expose LiveKit::livekit")
endif()

set(GCP_LIVEKIT_RESOLVED_PREFIX "${_gcp_sdk_prefix}")
message(STATUS "LiveKit native smoke test enabled with ${GCP_LIVEKIT_RESOLVED_PREFIX}")
