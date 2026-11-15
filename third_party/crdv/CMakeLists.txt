# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20)
project(crdv VERSION 0.2.0 LANGUAGES C)

option(CRDV_BUILD_TESTS "Build deterministic clean-room tests" ON)
option(CRDV_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(crdv STATIC
    src/air.c
    src/config.c
    src/control.c
    src/dv3000.c
    src/modem.c
    src/vita.c)
add_library(crdv::crdv ALIAS crdv)

target_include_directories(crdv PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(crdv PUBLIC c_std_17)
set_target_properties(crdv PROPERTIES C_EXTENSIONS OFF)

if(MSVC)
  target_compile_options(crdv PRIVATE /W4 /permissive-)
else()
  target_compile_options(crdv PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  target_link_libraries(crdv PUBLIC m)
  if(CRDV_ENABLE_SANITIZERS)
    target_compile_options(crdv PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(crdv PUBLIC -fsanitize=address,undefined)
  endif()
endif()

if(CRDV_BUILD_TESTS)
  enable_testing()
  add_executable(crdv_tests tests/test_main.c)
  target_link_libraries(crdv_tests PRIVATE crdv::crdv)
  target_compile_features(crdv_tests PRIVATE c_std_17)
  if(MSVC)
    target_compile_options(crdv_tests PRIVATE /W4 /permissive-)
  else()
    target_compile_options(crdv_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  endif()
  add_test(NAME crdv_tests COMMAND crdv_tests)

  add_executable(crdv_quarantined tests/quarantined_acceptance.c)
  target_link_libraries(crdv_quarantined PRIVATE crdv::crdv)
  target_compile_features(crdv_quarantined PRIVATE c_std_17)
  add_test(NAME crdv_quarantined COMMAND crdv_quarantined)
  set_tests_properties(crdv_quarantined PROPERTIES SKIP_RETURN_CODE 77)
endif()

include(GNUInstallDirs)
install(TARGETS crdv EXPORT crdvTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/crdv.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT crdvTargets NAMESPACE crdv:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/crdv)
