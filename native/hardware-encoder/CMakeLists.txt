add_executable(gcp-hardware-h264
    src/main.cpp
    src/gpu_color_converter.cpp
    src/live_monitor_capture.cpp
)
target_compile_features(gcp-hardware-h264 PRIVATE cxx_std_20)
target_compile_definitions(gcp-hardware-h264 PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
if(MSVC)
    target_compile_options(gcp-hardware-h264 PRIVATE /W4 /permissive- /EHsc)
endif()
target_link_libraries(gcp-hardware-h264 PRIVATE
    d3d11 dxgi ole32 uuid mfplat mf mfuuid windowsapp)

if(BUILD_TESTING)
    add_test(NAME hardware_h264_cli_self_test
        COMMAND $<TARGET_FILE:gcp-hardware-h264> --self-test)
    add_test(NAME hardware_h264_help
        COMMAND $<TARGET_FILE:gcp-hardware-h264> --help)
endif()
