cmake_minimum_required(VERSION 3.25)

# Android companion spike — throwaway Phase-0/1 prototype (see README.md).
# Self-contained on purpose: does NOT link aethercore and is not added to
# the root build. Configure with the Qt-for-Android kit's qt-cmake.
project(AetherCompanionSpike VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick Network)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(aethercompanion
    src/main.cpp
    src/DiscoveryModel.cpp
    src/DiscoveryModel.h
)

qt_add_qml_module(aethercompanion
    URI AetherCompanion
    VERSION 1.0
    QML_FILES
        qml/Main.qml
)

set_target_properties(aethercompanion PROPERTIES
    QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android"
    QT_ANDROID_MIN_SDK_VERSION 28
    QT_ANDROID_TARGET_SDK_VERSION 35
)

target_link_libraries(aethercompanion PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::Network
)
