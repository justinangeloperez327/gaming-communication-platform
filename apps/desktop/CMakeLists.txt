qt_add_executable(gcp-desktop
    src/main.cpp
    src/VoiceSession.cpp
    src/VoiceSession.h
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
)

set_target_properties(gcp-desktop PROPERTIES
    WIN32_EXECUTABLE TRUE
)

if(BUILD_TESTING)
    add_test(NAME desktop_binary_smoke
        COMMAND $<TARGET_FILE:gcp-desktop> --self-test
    )
endif()
