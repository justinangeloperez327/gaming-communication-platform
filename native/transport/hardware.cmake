# Optional GPU hardware H.264 stream to LiveKit.
add_executable(gcp-livekit-hardware
    src/hardware_main.cpp
    src/h264_access_unit.cpp
    ../hardware-encoder/src/main.cpp
    ../hardware-encoder/src/gpu_color_converter.cpp
    ../hardware-encoder/src/live_monitor_capture.cpp
    ../probe/src/telemetry.cpp
)
target_include_directories(gcp-livekit-hardware PRIVATE ../hardware-encoder/src)
target_compile_features(gcp-livekit-hardware PRIVATE cxx_std_20)
target_compile_definitions(gcp-livekit-hardware PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00
    GCP_HARDWARE_H264_NO_MAIN)
if(MSVC)
    target_compile_options(gcp-livekit-hardware PRIVATE /W4 /permissive- /EHsc)
endif()
target_link_libraries(gcp-livekit-hardware PRIVATE
    LiveKit::livekit d3d11 dxgi mfplat mf mfuuid ole32 uuid windowsapp psapi)
target_link_directories(gcp-livekit-hardware PRIVATE "${GCP_LIVEKIT_RESOLVED_PREFIX}/lib")

foreach(_sdk_runtime IN ITEMS livekit.dll livekit_ffi.dll)
    if(EXISTS "${GCP_LIVEKIT_RESOLVED_PREFIX}/bin/${_sdk_runtime}")
        add_custom_command(TARGET gcp-livekit-hardware POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GCP_LIVEKIT_RESOLVED_PREFIX}/bin/${_sdk_runtime}"
                "$<TARGET_FILE_DIR:gcp-livekit-hardware>"
            VERBATIM)
    endif()
endforeach()

if(BUILD_TESTING)
    add_test(NAME livekit_hardware_args
        COMMAND $<TARGET_FILE:gcp-livekit-hardware> --self-test)
    add_test(NAME livekit_hardware_help
        COMMAND $<TARGET_FILE:gcp-livekit-hardware> --help)
endif()
