cmake_minimum_required(VERSION 3.24)

project(GamingCommunicationPlatform VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

find_package(Qt6 6.5 REQUIRED COMPONENTS Quick QuickControls2)
qt_standard_project_setup(REQUIRES 6.5)

add_subdirectory(apps/desktop)

# Native media probes deliberately stay independent of Qt's UI and of any SFU.
# Keep them Windows-only until actual platform ports have been implemented.
if(WIN32)
    add_subdirectory(native/probe)
endif()
