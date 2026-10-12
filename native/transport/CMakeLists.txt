add_executable(gcp-livekit-smoke
    src/main.cpp
    ../probe/src/telemetry.cpp
)

target_include_directories(gcp-livekit-smoke PRIVATE ../probe/src)
target_compile_definitions(gcp-livekit-smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    target_compile_options(gcp-livekit-smoke PRIVATE /W4 /permissive- /EHsc)
endif()

target_link_libraries(gcp-livekit-smoke PRIVATE LiveKit::livekit psapi)

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
