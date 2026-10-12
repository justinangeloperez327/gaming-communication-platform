add_executable(gcp-media-probe
    src/main.cpp
    src/audio_probe.cpp
    src/capture_probe.cpp
    src/telemetry.cpp
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
    avrt mmdevapi propsys psapi
)

if(BUILD_TESTING)
    add_test(NAME media_probe_argument_tests
        COMMAND $<TARGET_FILE:gcp-media-probe> --self-test)
    add_test(NAME media_probe_help
        COMMAND $<TARGET_FILE:gcp-media-probe> --help)
endif()
