# build_identity_capture_test (#5804): drives cmake/AetherBuildIdentity.cmake the
# way the aether_build_identity target does on every build, against a scratch git
# repository, and checks the generated header in each state a checkout can be in.
#
# The case that matters most is "new commit, no re-configure": the header must
# follow HEAD, because that is exactly where the configure-time AETHER_GIT_SHA
# goes stale. The case that keeps it cheap is "nothing changed": the header must
# not be rewritten, or every build would recompile what includes it.
#
#   cmake -DAETHER_SOURCE_DIR=<repo> -DWORK_DIR=<scratch> -P build_identity_capture_test.cmake

if(NOT AETHER_SOURCE_DIR OR NOT WORK_DIR)
    message(FATAL_ERROR "usage: -DAETHER_SOURCE_DIR=<repo> -DWORK_DIR=<scratch>")
endif()

find_package(Git QUIET)
if(NOT Git_FOUND)
    # The capture degrades to "unknown" without git; nothing further to drive.
    message(STATUS "SKIP git not found")
    return()
endif()

set(_script "${AETHER_SOURCE_DIR}/cmake/AetherBuildIdentity.cmake")
set(_template "${AETHER_SOURCE_DIR}/src/core/AetherBuildIdentity.h.in")
set(_repo "${WORK_DIR}/repo")
set(_nogit "${WORK_DIR}/not-a-checkout")
set(_header "${WORK_DIR}/AetherBuildIdentity.h")

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${_repo}" "${_nogit}")

# Keep the scratch repository independent of whoever runs the test: no global
# or system config (signing, hooks, default branch), a fixed identity.
file(WRITE "${WORK_DIR}/empty.gitconfig" "")
set(ENV{GIT_CONFIG_GLOBAL} "${WORK_DIR}/empty.gitconfig")
set(ENV{GIT_CONFIG_NOSYSTEM} "1")
# The scratch tree usually sits inside a build directory inside the AetherSDR
# checkout; stop git from finding THAT repository above it.
set(ENV{GIT_CEILING_DIRECTORIES} "${WORK_DIR}")
set(ENV{GIT_AUTHOR_NAME} "t")
set(ENV{GIT_AUTHOR_EMAIL} "t@example.invalid")
set(ENV{GIT_COMMITTER_NAME} "t")
set(ENV{GIT_COMMITTER_EMAIL} "t@example.invalid")

set(_failures 0)

function(git_in)
    execute_process(COMMAND ${GIT_EXECUTABLE} ${ARGN}
                    WORKING_DIRECTORY "${_repo}"
                    RESULT_VARIABLE _rv OUTPUT_QUIET ERROR_VARIABLE _err)
    if(NOT _rv EQUAL 0)
        message(FATAL_ERROR "git ${ARGN} failed: ${_err}")
    endif()
endfunction()

function(head_sha out)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
                    WORKING_DIRECTORY "${_repo}"
                    OUTPUT_VARIABLE _s OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(${out} "${_s}" PARENT_SCOPE)
endfunction()

# Run the capture exactly as the build target does.
function(capture src)
    execute_process(COMMAND ${CMAKE_COMMAND}
                        -DAETHER_SRC_DIR=${src}
                        -DAETHER_IN_FILE=${_template}
                        -DAETHER_OUT_FILE=${_header}
                        -P ${_script}
                    RESULT_VARIABLE _rv)
    if(NOT _rv EQUAL 0)
        message(FATAL_ERROR "capture script failed in ${src}")
    endif()
endfunction()

# expect(<label> <macro> <expected>) -- <expected> is the literal right-hand side
# of the #define, quotes included for strings.
function(expect label macro expected)
    file(STRINGS "${_header}" _line REGEX "^#define ${macro} ")
    string(REGEX REPLACE "^#define ${macro} +" "" _actual "${_line}")
    if("${_actual}" STREQUAL "${expected}")
        message(STATUS "PASS ${label}: ${macro} = ${_actual}")
    else()
        message(STATUS "FAIL ${label}: ${macro} = ${_actual}, expected ${expected}")
        math(EXPR _f "${_failures} + 1")
        set(_failures ${_f} PARENT_SCOPE)
    endif()
endfunction()

# ---- 1. Not a git checkout (a source tarball) ------------------------------
capture("${_nogit}")
expect("no checkout" AETHER_BUILD_DESCRIBE "\"unknown\"")
expect("no checkout" AETHER_BUILD_SHA "\"unknown\"")
expect("no checkout" AETHER_BUILD_BASELINE "\"unknown\"")
expect("no checkout" AETHER_BUILD_COMMITS_SINCE_TAG "-1")
expect("no checkout" AETHER_BUILD_DIRTY "false")

# ---- 2. A checkout with no reachable tag (a shallow CI clone) --------------
git_in(init -q)
file(WRITE "${_repo}/file.txt" "one\n")
git_in(add file.txt)
git_in(commit -q -m one)
head_sha(_sha1)
capture("${_repo}")
expect("no tag" AETHER_BUILD_DESCRIBE "\"${_sha1}\"")
expect("no tag" AETHER_BUILD_SHA "\"${_sha1}\"")
expect("no tag" AETHER_BUILD_BASELINE "\"unknown\"")
expect("no tag" AETHER_BUILD_COMMITS_SINCE_TAG "-1")
expect("no tag" AETHER_BUILD_DIRTY "false")

# ---- 3. HEAD exactly on a tag ----------------------------------------------
git_in(tag v1.2.3)
capture("${_repo}")
expect("on tag" AETHER_BUILD_DESCRIBE "\"v1.2.3\"")
expect("on tag" AETHER_BUILD_SHA "\"${_sha1}\"")
expect("on tag" AETHER_BUILD_BASELINE "\"v1.2.3\"")
expect("on tag" AETHER_BUILD_COMMITS_SINCE_TAG "0")
expect("on tag" AETHER_BUILD_DIRTY "false")

# ---- 4. New commits, NO re-configure: the header must follow HEAD ----------
file(WRITE "${_repo}/file.txt" "two\n")
git_in(commit -q -am two)
file(WRITE "${_repo}/file.txt" "three\n")
git_in(commit -q -am three)
head_sha(_sha3)
capture("${_repo}")
expect("past tag" AETHER_BUILD_DESCRIBE "\"v1.2.3-2-g${_sha3}\"")
expect("past tag" AETHER_BUILD_SHA "\"${_sha3}\"")
expect("past tag" AETHER_BUILD_BASELINE "\"v1.2.3\"")
expect("past tag" AETHER_BUILD_COMMITS_SINCE_TAG "2")
expect("past tag" AETHER_BUILD_DIRTY "false")

# ---- 5. Unchanged HEAD: the header is not rewritten ------------------------
file(TIMESTAMP "${_header}" _before "%s" UTC)
file(SHA256 "${_header}" _hash_before)
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1.2)
capture("${_repo}")
file(TIMESTAMP "${_header}" _after "%s" UTC)
file(SHA256 "${_header}" _hash_after)
if(_before STREQUAL _after AND _hash_before STREQUAL _hash_after)
    message(STATUS "PASS unchanged HEAD leaves the header untouched (mtime ${_after})")
else()
    message(STATUS "FAIL unchanged HEAD rewrote the header (${_before} -> ${_after})")
    math(EXPR _failures "${_failures} + 1")
endif()

# ---- 6. A modified tracked file marks the build dirty ----------------------
file(WRITE "${_repo}/file.txt" "uncommitted\n")
capture("${_repo}")
expect("dirty" AETHER_BUILD_DESCRIBE "\"v1.2.3-2-g${_sha3}-dirty\"")
expect("dirty" AETHER_BUILD_SHA "\"${_sha3}\"")
expect("dirty" AETHER_BUILD_BASELINE "\"v1.2.3\"")
expect("dirty" AETHER_BUILD_COMMITS_SINCE_TAG "2")
expect("dirty" AETHER_BUILD_DIRTY "true")

# ---- 7. An untracked file does not ------------------------------------------
git_in(checkout -q -- file.txt)
file(WRITE "${_repo}/untracked.txt" "x\n")
capture("${_repo}")
expect("untracked only" AETHER_BUILD_DIRTY "false")

file(REMOVE_RECURSE "${WORK_DIR}")

if(_failures GREATER 0)
    message(FATAL_ERROR "${_failures} check(s) failed")
endif()
message(STATUS "all build identity capture checks passed")
