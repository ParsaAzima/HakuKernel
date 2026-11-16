# CMake is the test front end, not the compiler. build.py still owns the
# image pipeline (NASM, clang-cl, lld-link, mkfat12/mkcpio, QEMU), because the
# steps feed each other through generated paths, entry addresses and disk
# images. Registering those steps here gives CI and a workstation the same
# `ctest` names and one place that lists every validation gate.
#
#   cmake -S . -B build/ctest
#   ctest --test-dir build/ctest --output-on-failure
#   ctest --test-dir build/ctest -R "^i386"
#
# No C or C++ language is enabled on purpose: nothing is compiled by CMake.
cmake_minimum_required(VERSION 3.20)

project(HakuKernel LANGUAGES NONE)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(HAKU_BUILD
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build.py")

# Convenience build targets. They are deliberately not part of ALL: a plain
# `cmake --build` stays a no-op and the boot tests build exactly what they run.
add_custom_target(haku-x64
    COMMAND ${HAKU_BUILD} build
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL
    COMMENT "Building haku64.img (x86-64)")

add_custom_target(haku-i386
    COMMAND ${HAKU_BUILD} build --arch=i386
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL
    COMMENT "Building haku.img (i386)")

include(CTest)

if(BUILD_TESTING)
    # Every gate builds an image, boots it in QEMU on a fixed monitor port and
    # validates the serial log, so the tests must not overlap. The timeout is
    # a backstop for a wedged emulator; the per-run waits inside build.py are
    # shorter and report which marker is missing.
    function(haku_add_boot_test name)
        add_test(NAME ${name}
                 COMMAND ${HAKU_BUILD} headless ${ARGN})
        set_tests_properties(${name} PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            TIMEOUT 1800
            RUN_SERIAL TRUE
            LABELS "boot")
    endfunction()

    haku_add_boot_test(x64)
    haku_add_boot_test(x64-smp4 --smp=4)
    haku_add_boot_test(i386 --arch=i386)
    haku_add_boot_test(i386-smp4 --arch=i386 --smp=4)
endif()
