cmake_minimum_required(VERSION 3.24)
project(GcpDesktopChatApiAcceptance LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Network WebSockets Quick QuickControls2)
add_executable(gcp-chat-api-acceptance
    chat_api_acceptance.cpp
    ../../src/ServerProbe.cpp ../../src/ServerProbe.h
    ../../src/ScreenViewer.cpp ../../src/ScreenViewer.h
    ../../src/ScreenSession.cpp ../../src/ScreenSession.h
    ../../src/DesktopSettings.cpp ../../src/DesktopSettings.h
    ../../src/AccountSession.cpp ../../src/AccountSession.h
    ../../src/ChannelDirectory.cpp ../../src/ChannelDirectory.h
    ../../src/ChatSession.cpp ../../src/ChatSession.h
    ../../src/MessageModel.cpp ../../src/MessageModel.h
    ../../src/FriendsDirectory.cpp ../../src/FriendsDirectory.h)
target_include_directories(gcp-chat-api-acceptance PRIVATE ../../src)
target_link_libraries(gcp-chat-api-acceptance PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::WebSockets)

add_executable(gcp-screen-process-fixture ../../src/screen_process_fixture.cpp)

add_executable(gcp-view-process-fixture ../../src/view_process_fixture.cpp)

enable_testing()
add_executable(gcp-desktop-settings-tests
    ../../src/settings_tests.cpp ../../src/DesktopSettings.cpp ../../src/DesktopSettings.h
    ../../src/ServerProbe.cpp ../../src/ServerProbe.h ../../src/AccountSession.cpp ../../src/AccountSession.h
    ../../src/VoiceSession.cpp ../../src/VoiceSession.h ../../src/ChannelDirectory.cpp ../../src/ChannelDirectory.h)
target_link_libraries(gcp-desktop-settings-tests PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::Network)
target_compile_definitions(gcp-desktop-settings-tests PRIVATE GCP_SETTINGS_QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../qml")
add_test(NAME desktop_preferences_and_recovery COMMAND $<TARGET_FILE:gcp-desktop-settings-tests>)
set_tests_properties(desktop_preferences_and_recovery PROPERTIES
    TIMEOUT 35 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
