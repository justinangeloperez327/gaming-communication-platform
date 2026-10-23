cmake_minimum_required(VERSION 3.24)
project(GcpDesktopChatApiAcceptance LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network WebSockets)
add_executable(gcp-chat-api-acceptance
    chat_api_acceptance.cpp
    ../../src/AccountSession.cpp ../../src/AccountSession.h
    ../../src/ChannelDirectory.cpp ../../src/ChannelDirectory.h
    ../../src/ChatSession.cpp ../../src/ChatSession.h
    ../../src/MessageModel.cpp ../../src/MessageModel.h
    ../../src/FriendsDirectory.cpp ../../src/FriendsDirectory.h)
target_include_directories(gcp-chat-api-acceptance PRIVATE ../../src)
target_link_libraries(gcp-chat-api-acceptance PRIVATE Qt6::Core Qt6::Network Qt6::WebSockets)
