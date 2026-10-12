add_executable(gcp-media-probe
    src/main.cpp
    src/audio_probe.cpp
    src/capture_probe.cpp
    src/telemetry.cpp
    src/encoder_capabilities.cpp
    src/copy_benchmark.cpp
)

target_include_directories(gcp-media-probe PRIVATE src)
target_compile_definitions(gcp-media-probe PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
)

if(MSVC)
    target_compile_options(gcp-media-probe PRIVATE /W4 /permissive- /EHsc)
endif()

target_link_libraries(gcp-media-probe PRIVATE
    d3d11 dxgi windowsapp ole32 uuid
    avrt propsys psapi mfplat mf mfuuid
)

if(BUILD_TESTING)
    add_test(NAME media_probe_argument_tests
        COMMAND $<TARGET_FILE:gcp-media-probe> --self-test)
    add_test(NAME media_probe_help
        COMMAND $<TARGET_FILE:gcp-media-probe> --help)
    add_test(NAME media_probe_copy_benchmark
        COMMAND $<TARGET_FILE:gcp-media-probe> --benchmark-copy
                --width 640 --height 360 --iterations 32)
endif()
