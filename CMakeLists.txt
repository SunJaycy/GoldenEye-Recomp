
# ge - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(ge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(GE_SOURCES
        src/main.cpp
        src/ge_hooks.cpp
        src/ge_menu.cpp
        src/ge_postfx.cpp
        src/ge_username.cpp
        src/ge_ce_patches.cpp
)

if(WIN32)
    # Embed the application icon (GoldenEye.rc -> "MAINICON"). Use LLVM's resource
    # compiler since the toolchain is clang/lld (MSVC rc.exe may not be on PATH).
    if(NOT CMAKE_RC_COMPILER)
        set(CMAKE_RC_COMPILER "llvm-rc")
    endif()
    enable_language(RC)
    add_executable(ge WIN32 ${GE_SOURCES} GoldenEye.rc)
else()
    add_executable(ge ${GE_SOURCES})
endif()

# Output a GoldenEye.exe (the in-Explorer/process name), independent of the
# internal target/app name "ge" (which still drives ge.toml / the user data dir).
set_target_properties(ge PROPERTIES OUTPUT_NAME "GoldenEye")

rexglue_setup_target(ge)

if(UNIX AND NOT APPLE)
    # ge_hooks.cpp drives mouse-look through Xlib (pointer grab + warp) and reads
    # held keys with XQueryKeymap -- the Win32 raw-input/GetAsyncKeyState path has
    # no portable equivalent. The SDK's own Linux window is GTK with GDK_BACKEND
    # pinned to x11, so an X server is always present.
    find_package(X11 REQUIRED)
    target_link_libraries(ge PRIVATE X11::X11)

    # The SDK's POST_BUILD runtime-DLL staging is Windows-only, but the target
    # is linked with RPATH=$ORIGIN, so on Linux the shared libraries have to be
    # copied next to the executable or it won't start ("librexruntime.so => not
    # found"). Do it here so a clean build is runnable without manual steps.
    foreach(_lib rex::runtime rex::TracyClient)
        if(TARGET ${_lib})
            add_custom_command(TARGET ge POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:${_lib}> $<TARGET_FILE_DIR:ge>
                    VERBATIM)
        endif()
    endforeach()
    # SPIRV-Tools is a transitive dependency of the runtime rather than a
    # target we link, so copy it by path from the SDK's library directory.
    get_target_property(_rt_loc rex::runtime IMPORTED_LOCATION_RELEASE)
    if(NOT _rt_loc)
        get_target_property(_rt_loc rex::runtime IMPORTED_LOCATION)
    endif()
    if(_rt_loc)
        get_filename_component(_rt_dir "${_rt_loc}" DIRECTORY)
        file(GLOB _spirv "${_rt_dir}/libSPIRV-Tools-shared.so*")
        if(_spirv)
            add_custom_command(TARGET ge POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${_spirv} $<TARGET_FILE_DIR:ge>
                    VERBATIM)
        endif()
    endif()
endif()
