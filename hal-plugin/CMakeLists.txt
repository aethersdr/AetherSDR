cmake_minimum_required(VERSION 3.20)
project(MasterSDRDAX LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetch libASPL
include(FetchContent)
FetchContent_Declare(libASPL
    GIT_REPOSITORY https://github.com/gavv/libASPL.git
    GIT_TAG        v3.1.2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(libASPL)

# Build the plugin as a loadable bundle (.driver)
add_library(MasterSDRDAX MODULE MasterSDRDAX.cpp)
target_link_libraries(MasterSDRDAX PRIVATE libASPL)

# Include shared header from parent project
target_include_directories(MasterSDRDAX PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

set_target_properties(MasterSDRDAX PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION "driver"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    # Don't add lib prefix
    PREFIX ""
)

# Install to the HAL plug-in directory (requires sudo)
install(TARGETS MasterSDRDAX
    DESTINATION "/Library/Audio/Plug-Ins/HAL"
)
