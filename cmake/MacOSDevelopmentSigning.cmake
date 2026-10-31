option(AETHER_MACOS_ADHOC_SIGN
    "Ad-hoc sign local macOS development bundles; not for distribution signing" OFF)

function(aether_sign_macos_development_bundle target)
    if(NOT APPLE OR NOT AETHER_MACOS_ADHOC_SIGN OR CMAKE_GENERATOR STREQUAL "Xcode")
        return()
    endif()

    find_program(AETHER_CODESIGN_EXECUTABLE codesign REQUIRED)
    # Intel linkers leave nested code unsigned. Seal it before the outer bundle.
    foreach(nested_code IN LISTS ARGN)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${AETHER_CODESIGN_EXECUTABLE}" --force --sign - "${nested_code}"
            VERBATIM)
    endforeach()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${AETHER_CODESIGN_EXECUTABLE}" --force --sign -
            --identifier "$<TARGET_PROPERTY:${target},MACOSX_BUNDLE_GUI_IDENTIFIER>"
            "$<TARGET_BUNDLE_DIR:${target}>"
        COMMAND "${AETHER_CODESIGN_EXECUTABLE}" --verify --deep --strict
            "$<TARGET_BUNDLE_DIR:${target}>"
        COMMENT "Ad-hoc signing the macOS development bundle"
        VERBATIM)
endfunction()
