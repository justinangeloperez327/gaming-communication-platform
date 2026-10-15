qt_add_executable(gcp-desktop
    src/main.cpp
    src/VoiceSession.cpp
    src/VoiceSession.h
    src/AccountSession.cpp
    src/AccountSession.h
    src/ChannelDirectory.cpp
    src/ChannelDirectory.h
)

set_source_files_properties(qml/Main.qml PROPERTIES
    QT_RESOURCE_ALIAS Main.qml
)

qt_add_qml_module(gcp-desktop
    URI Gcp.Desktop
    VERSION 1.0
    QML_FILES
        qml/Main.qml
)

target_link_libraries(gcp-desktop PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
)

set_target_properties(gcp-desktop PROPERTIES
    WIN32_EXECUTABLE TRUE
)

if(BUILD_TESTING)
    add_test(NAME desktop_binary_smoke
        COMMAND $<TARGET_FILE:gcp-desktop> --self-test
    )
endif()

if(BUILD_TESTING)
    qt_add_executable(gcp-desktop-network-tests
        src/network_tests.cpp src/AccountSession.cpp src/AccountSession.h)
    target_link_libraries(gcp-desktop-network-tests PRIVATE Qt6::Network)
    add_test(NAME desktop_network_limits_and_cancellation
        COMMAND $<TARGET_FILE:gcp-desktop-network-tests>)
    set_tests_properties(desktop_network_limits_and_cancellation PROPERTIES TIMEOUT 15)
endif()
