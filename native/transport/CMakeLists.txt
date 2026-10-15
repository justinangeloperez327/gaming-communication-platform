add_executable(gcp-livekit-smoke
    src/main.cpp
    src/annexb_h264.cpp
    src/screen_capture.cpp
    ../probe/src/telemetry.cpp
)

target_include_directories(gcp-livekit-smoke PRIVATE ../probe/src)
target_compile_definitions(gcp-livekit-smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    target_compile_options(gcp-livekit-smoke PRIVATE /W4 /permissive- /EHsc)
endif()

target_link_libraries(gcp-livekit-smoke PRIVATE LiveKit::livekit psapi d3d11 dxgi windowsapp ole32 uuid)

# The LiveKit imported target references the bundled FFI library on Windows.
target_link_directories(gcp-livekit-smoke PRIVATE
    "${GCP_LIVEKIT_RESOLVED_PREFIX}/lib")

# These are runtime dependencies, not bundled in the repository. They are
# copied only when a developer explicitly opts into a LiveKit build.
foreach(_sdk_runtime IN ITEMS livekit.dll livekit_ffi.dll)
    if(EXISTS "${GCP_LIVEKIT_RESOLVED_PREFIX}/bin/${_sdk_runtime}")
        add_custom_command(TARGET gcp-livekit-smoke POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GCP_LIVEKIT_RESOLVED_PREFIX}/bin/${_sdk_runtime}"
                "$<TARGET_FILE_DIR:gcp-livekit-smoke>"
            VERBATIM
        )
    endif()
endforeach()

if(BUILD_TESTING)
    add_test(NAME livekit_smoke_args
        COMMAND $<TARGET_FILE:gcp-livekit-smoke> --self-test)
    add_test(NAME livekit_smoke_help
        COMMAND $<TARGET_FILE:gcp-livekit-smoke> --help)
endif()

# Optional GPU hardware encoded bridge.
include(${CMAKE_CURRENT_LIST_DIR}/hardware.cmake)

# Group 27: WebRTC platform audio capture and automatic speaker playback.
add_executable(gcp-livekit-voice
    src/voice_main.cpp
    ../probe/src/telemetry.cpp
)
target_include_directories(gcp-livekit-voice PRIVATE ../probe/src)
target_compile_features(gcp-livekit-voice PRIVATE cxx_std_20)
target_compile_definitions(gcp-livekit-voice PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(gcp-livekit-voice PRIVATE /W4 /permissive- /EHsc)
endif()
target_link_libraries(gcp-livekit-voice PRIVATE LiveKit::livekit psapi ole32 uuid)
target_link_directories(gcp-livekit-voice PRIVATE "${GCP_LIVEKIT_RESOLVED_PREFIX}/lib")
foreach(_runtime IN ITEMS livekit.dll livekit_ffi.dll)
    if(EXISTS "${GCP_LIVEKIT_RESOLVED_PREFIX}/bin/${_runtime}")
        add_custom_command(TARGET gcp-livekit-voice POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GCP_LIVEKIT_RESOLVED_PREFIX}/bin/${_runtime}"
                "$<TARGET_FILE_DIR:gcp-livekit-voice>"
            VERBATIM)
    endif()
endforeach()
if(BUILD_TESTING)
    add_test(NAME livekit_voice_consent_and_sdk
        COMMAND $<TARGET_FILE:gcp-livekit-voice> --self-test)
    add_test(NAME livekit_voice_help
        COMMAND $<TARGET_FILE:gcp-livekit-voice> --help)
endif()
