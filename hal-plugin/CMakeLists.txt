cmake_minimum_required(VERSION 3.20)
project(AetherSDRDAX LANGUAGES CXX)

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
add_library(AetherSDRDAX MODULE AetherSDRDAX.cpp)
target_link_libraries(AetherSDRDAX PRIVATE libASPL)

# Include shared header from parent project
target_include_directories(AetherSDRDAX PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# The driver's source (RFC #5304 Phase 4): the git tree hash of hal-plugin/,
# stamped into its Info.plist as AetherSDRDAXSource. The app reads the
# installed driver's stamp and compares it with the one the update manifest
# names, to say a release changes the driver. Empty outside a git checkout.
set(AETHERSDR_DAX_SOURCE "")
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse HEAD:hal-plugin
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
        OUTPUT_VARIABLE _aether_dax_source OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE _aether_dax_rv)
    if(_aether_dax_rv EQUAL 0 AND _aether_dax_source MATCHES "^[0-9a-f]+$")
        set(AETHERSDR_DAX_SOURCE "${_aether_dax_source}")
    endif()
endif()

set_target_properties(AetherSDRDAX PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION "driver"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    # Don't add lib prefix
    PREFIX ""
)

# Install to the HAL plug-in directory (requires sudo)
# Install only the runtime driver in the DAX package. FetchContent also
# defines libASPL's developer-file install rules; those must stay out.
install(TARGETS AetherSDRDAX
    DESTINATION "/Library/Audio/Plug-Ins/HAL"
    COMPONENT dax_driver
)
