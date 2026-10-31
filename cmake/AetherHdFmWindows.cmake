# nrsc5 alone needs GNU C complex arithmetic and VLAs. Keep the application and
# HDC decoder on MSVC, and compile these C objects with clang-cl's matching ABI.
# No compiler/runtime download or global compiler/flag change is performed.
include_guard(GLOBAL)

macro(aether_hd_windows_compiler)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
        message(FATAL_ERROR "ENABLE_HD_FM Windows qualification supports x64 only.")
    endif()
    if(CMAKE_MSVC_RUNTIME_LIBRARY AND NOT CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        message(FATAL_ERROR "ENABLE_HD_FM Windows requires the shared MSVC runtime (/MD or /MDd).")
    endif()
    find_program(AETHER_HD_CLANG_CL NAMES clang-cl HINTS "C:/Program Files/LLVM/bin" REQUIRED)
    execute_process(COMMAND "${AETHER_HD_CLANG_CL}" --target=x86_64-pc-windows-msvc
        /clang:--rtlib=compiler-rt /clang:-print-libgcc-file-name
        RESULT_VARIABLE runtime_result OUTPUT_VARIABLE AETHER_HD_COMPILER_RT
        ERROR_VARIABLE runtime_error OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT runtime_result EQUAL 0 OR NOT EXISTS "${AETHER_HD_COMPILER_RT}")
        message(FATAL_ERROR "ENABLE_HD_FM requires the installed LLVM x64 compiler-rt builtins: ${runtime_error}")
    endif()
    file(TO_CMAKE_PATH "${AETHER_HD_COMPILER_RT}" AETHER_HD_COMPILER_RT)
    set(AETHER_HD_COMPILER_RT "${AETHER_HD_COMPILER_RT}" CACHE INTERNAL "Installed LLVM complex arithmetic builtins" FORCE)
endmacro()

function(aether_hd_windows_c_objects output_variable object_group sources includes definitions)
    get_filename_component(vendor_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party" ABSOLUTE)
    set(faad_root "${vendor_root}/faad_hdc/upstream")
    set(config_root "${CMAKE_BINARY_DIR}/aether-hd-fm")
    set(object_dir "${config_root}/${object_group}/$<CONFIG>")
    set(include_flags)
    foreach(include_dir IN LISTS includes)
        list(APPEND include_flags "-I${include_dir}")
    endforeach()
    set(definition_flags)
    foreach(definition IN LISTS definitions)
        list(APPEND definition_flags "-D${definition}")
    endforeach()
    # This finite vendored subtree is small. Rebuild on every header change,
    # including generated config and private compatibility headers.
    file(GLOB_RECURSE nrsc5_headers CONFIGURE_DEPENDS
        "${vendor_root}/nrsc5/*.h" "${faad_root}/include/*.h")
    foreach(include_dir IN LISTS includes)
        file(GLOB dependency_headers CONFIGURE_DEPENDS "${include_dir}/*.h")
        list(APPEND nrsc5_headers ${dependency_headers})
    endforeach()
    set(objects)
    foreach(source IN LISTS sources)
        get_filename_component(source_name "${source}" NAME_WE)
        set(object "${object_dir}/${source_name}.obj")
        add_custom_command(OUTPUT "${object}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${object_dir}"
            COMMAND "${AETHER_HD_CLANG_CL}" /nologo --target=x86_64-pc-windows-msvc
                /clang:-std=gnu11 "$<IF:$<CONFIG:Debug>,/MDd,/MD>"
                "$<IF:$<CONFIG:Debug>,/Od,/O2>" /Z7 /W3
                -D_USE_MATH_DEFINES -D_CRT_SECURE_NO_WARNINGS
                ${include_flags} ${definition_flags} /c "${source}" "/Fo${object}"
            DEPENDS "${source}" ${nrsc5_headers} "${config_root}/config.h"
            VERBATIM COMMENT "Building native HD C object ${object_group}/${source_name}")
        list(APPEND objects "${object}")
    endforeach()
    set_source_files_properties(${objects} PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
    set(${output_variable} "${objects}" PARENT_SCOPE)
endfunction()

function(aether_hd_windows_nrsc5 sources)
    get_target_property(rtl_includes "${RTLSDR_TARGET}" INTERFACE_INCLUDE_DIRECTORIES)
    get_target_property(fftw_includes "${RTL_FFTW3F_TARGET}" INTERFACE_INCLUDE_DIRECTORIES)
    set(includes "${vendor_root}/nrsc5/compat/msvc" "${config_root}"
        "${nrsc5_root}/include" "${faad_root}/include" ${rtl_includes} ${fftw_includes})
    set(definitions HAVE_FAAD2=1 FFTW_DLL= GIT_COMMIT_HASH="0225922b6f68109df39d07391f4d855464598ab8")
    aether_hd_windows_c_objects(objects nrsc5 "${sources}" "${includes}" "${definitions}")
    add_library(aether_nrsc5 STATIC ${objects})
    set_target_properties(aether_nrsc5 PROPERTIES LINKER_LANGUAGE C)
    target_link_libraries(aether_nrsc5 PRIVATE "${AETHER_HD_COMPILER_RT}" ws2_32)
    message(STATUS "HD FM Windows C compiler: ${AETHER_HD_CLANG_CL}; runtime: ${AETHER_HD_COMPILER_RT}")
endfunction()
