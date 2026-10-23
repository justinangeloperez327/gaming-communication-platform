qt_add_executable(gcp-desktop
    src/main.cpp
    src/VoiceSession.cpp
    src/VoiceSession.h
    src/AccountSession.cpp
    src/AccountSession.h
    src/ChannelDirectory.cpp
    src/ChannelDirectory.h
    src/ChatSession.cpp src/ChatSession.h src/MessageModel.cpp src/MessageModel.h
    src/FriendsDirectory.cpp src/FriendsDirectory.h src/ChatProtocol.h
)

set_source_files_properties(qml/Main.qml PROPERTIES
    QT_RESOURCE_ALIAS Main.qml
)
set_source_files_properties(qml/AudioDevicePicker.qml PROPERTIES
    QT_RESOURCE_ALIAS AudioDevicePicker.qml
)
set_source_files_properties(qml/IdPicker.qml PROPERTIES
    QT_RESOURCE_ALIAS IdPicker.qml
)
set_source_files_properties(qml/AccountPanel.qml PROPERTIES QT_RESOURCE_ALIAS AccountPanel.qml)
set_source_files_properties(qml/CommunityPanel.qml PROPERTIES QT_RESOURCE_ALIAS CommunityPanel.qml)
set_source_files_properties(qml/VoicePanel.qml PROPERTIES QT_RESOURCE_ALIAS VoicePanel.qml)
set_source_files_properties(qml/ChatPanel.qml PROPERTIES QT_RESOURCE_ALIAS ChatPanel.qml)
set_source_files_properties(qml/FriendsPanel.qml PROPERTIES QT_RESOURCE_ALIAS FriendsPanel.qml)

qt_add_qml_module(gcp-desktop
    URI Gcp.Desktop
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/AudioDevicePicker.qml
        qml/IdPicker.qml
        qml/AccountPanel.qml
        qml/CommunityPanel.qml
        qml/VoicePanel.qml
        qml/ChatPanel.qml
        qml/FriendsPanel.qml
)

target_link_libraries(gcp-desktop PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::WebSockets
)

set_target_properties(gcp-desktop PROPERTIES
    WIN32_EXECUTABLE TRUE
)

if(BUILD_TESTING)
    qt_add_executable(gcp-chat-tests
        src/chat_tests.cpp src/ChatSession.cpp src/ChatSession.h src/MessageModel.cpp src/MessageModel.h
        src/FriendsDirectory.cpp src/FriendsDirectory.h
        src/AccountSession.cpp src/AccountSession.h src/ChannelDirectory.cpp src/ChannelDirectory.h)
    target_link_libraries(gcp-chat-tests PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::Network Qt6::WebSockets)
    target_compile_definitions(gcp-chat-tests PRIVATE GCP_CHAT_QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    add_test(NAME desktop_text_messaging COMMAND $<TARGET_FILE:gcp-chat-tests>)
    set_tests_properties(desktop_text_messaging PROPERTIES
        TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

    qt_add_executable(gcp-community-tests
        src/community_tests.cpp src/ChannelDirectory.cpp src/ChannelDirectory.h
        src/AccountSession.cpp src/AccountSession.h)
    target_link_libraries(gcp-community-tests PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::Network)
    target_compile_definitions(gcp-community-tests PRIVATE
        GCP_COMMUNITY_QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    add_test(NAME desktop_community_management COMMAND $<TARGET_FILE:gcp-community-tests>)
    set_tests_properties(desktop_community_management PROPERTIES
        TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

    qt_add_executable(gcp-account-onboarding-tests
        src/onboarding_tests.cpp src/AccountSession.cpp src/AccountSession.h)
    target_link_libraries(gcp-account-onboarding-tests PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::Network)
    target_compile_definitions(gcp-account-onboarding-tests PRIVATE
        GCP_ACCOUNT_PANEL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/qml/AccountPanel.qml")
    add_test(NAME desktop_private_account_onboarding COMMAND $<TARGET_FILE:gcp-account-onboarding-tests>)
    set_tests_properties(desktop_private_account_onboarding PROPERTIES
        TIMEOUT 35 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

    qt_add_executable(gcp-audio-picker-tests src/audio_picker_tests.cpp)
    target_link_libraries(gcp-audio-picker-tests PRIVATE Qt6::Quick Qt6::QuickControls2)
    target_compile_definitions(gcp-audio-picker-tests PRIVATE
        GCP_AUDIO_PICKER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/qml/AudioDevicePicker.qml"
        GCP_ID_PICKER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/qml/IdPicker.qml")
    add_test(NAME desktop_audio_picker_selection COMMAND $<TARGET_FILE:gcp-audio-picker-tests>)
    set_tests_properties(desktop_audio_picker_selection PROPERTIES
        TIMEOUT 15 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

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

if(BUILD_TESTING)
    add_executable(gcp-voice-process-fixture src/voice_process_fixture.cpp)
    qt_add_executable(gcp-voice-session-tests
        src/voice_session_tests.cpp
        src/VoiceSession.cpp src/VoiceSession.h
        src/AccountSession.cpp src/AccountSession.h
        src/ChannelDirectory.cpp src/ChannelDirectory.h)
    target_link_libraries(gcp-voice-session-tests PRIVATE Qt6::Network Qt6::Quick Qt6::QuickControls2)
    target_compile_definitions(gcp-voice-session-tests PRIVATE
        GCP_VOICE_QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    add_dependencies(gcp-voice-session-tests gcp-voice-process-fixture)
    add_test(NAME desktop_voice_control_and_reauthorization
        COMMAND $<TARGET_FILE:gcp-voice-session-tests>)
    set_tests_properties(desktop_voice_control_and_reauthorization PROPERTIES
        TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
endif()
