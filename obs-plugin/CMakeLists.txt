cmake_minimum_required(VERSION 3.22)
project(obs-android-webcamera VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTS "Build unit and E2E tests" ON)

set(AWC_CORE_SOURCES
    src/awc-client.cpp
    src/awc-adb.cpp
    src/awc-decoder.cpp
)

add_library(obs-android-webcamera MODULE
    ${AWC_CORE_SOURCES}
    src/plugin-main.cpp
)

set_target_properties(obs-android-webcamera PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

if(MSVC)
    target_compile_options(obs-android-webcamera PRIVATE /W4 /utf-8)
    target_compile_definitions(obs-android-webcamera PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(obs-android-webcamera PRIVATE -Wall -Wextra -Wpedantic)
endif()

# 1. Locate libobs (prefer official OBS SDK / system package, fallback to bundled ABI header)
set(AWC_USING_SYSTEM_OBS FALSE)
find_package(libobs QUIET)
if(TARGET OBS::libobs)
    target_link_libraries(obs-android-webcamera PRIVATE OBS::libobs)
    set(AWC_USING_SYSTEM_OBS TRUE)
elseif(TARGET libobs)
    target_link_libraries(obs-android-webcamera PRIVATE libobs)
    set(AWC_USING_SYSTEM_OBS TRUE)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBOBS QUIET libobs)
    endif()
    if(LIBOBS_FOUND)
        target_include_directories(obs-android-webcamera PRIVATE ${LIBOBS_INCLUDE_DIRS})
        target_link_directories(obs-android-webcamera PRIVATE ${LIBOBS_LIBRARY_DIRS})
        target_link_libraries(obs-android-webcamera PRIVATE ${LIBOBS_LIBRARIES})
        set(AWC_USING_SYSTEM_OBS TRUE)
    elseif(OBS_INCLUDE_DIR)
        target_include_directories(obs-android-webcamera PRIVATE "${OBS_INCLUDE_DIR}")
        if(OBS_LIBRARY)
            target_link_libraries(obs-android-webcamera PRIVATE "${OBS_LIBRARY}")
        endif()
        set(AWC_USING_SYSTEM_OBS TRUE)
    endif()
endif()

if(NOT AWC_USING_SYSTEM_OBS)
    message(STATUS "System libobs not found; using bundled OBS 28-31 ABI compatibility headers")
    target_include_directories(obs-android-webcamera PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include/obs-compat"
    )
    if(WIN32)
        if(MSVC)
            set(OBS_IMPORT_LIB "${CMAKE_CURRENT_BINARY_DIR}/obs.lib")
            get_filename_component(MSVC_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
            find_program(MSVC_LIB_TOOL NAMES lib.exe lib HINTS "${MSVC_BIN_DIR}" REQUIRED)
            add_custom_command(
                OUTPUT "${OBS_IMPORT_LIB}"
                COMMAND "${MSVC_LIB_TOOL}" /NOLOGO "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/cmake/obs.def"
                        "/OUT:${OBS_IMPORT_LIB}" /MACHINE:X64
                DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/obs.def"
                COMMENT "Generating obs.lib import library from cmake/obs.def"
            )
            add_custom_target(awc_obs_import_lib DEPENDS "${OBS_IMPORT_LIB}")
            add_dependencies(obs-android-webcamera awc_obs_import_lib)
            target_link_libraries(obs-android-webcamera PRIVATE "${OBS_IMPORT_LIB}")
        else()
            set(OBS_IMPORT_LIB "${CMAKE_CURRENT_BINARY_DIR}/libobs.dll.a")
            find_program(DLLTOOL_BIN dlltool REQUIRED)
            add_custom_command(
                OUTPUT "${OBS_IMPORT_LIB}"
                COMMAND "${DLLTOOL_BIN}" -d "${CMAKE_CURRENT_SOURCE_DIR}/cmake/obs.def"
                        -l "${OBS_IMPORT_LIB}"
                DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/obs.def"
                COMMENT "Generating libobs.dll.a from cmake/obs.def"
            )
            add_custom_target(awc_obs_import_lib DEPENDS "${OBS_IMPORT_LIB}")
            add_dependencies(obs-android-webcamera awc_obs_import_lib)
            target_link_libraries(obs-android-webcamera PRIVATE "${OBS_IMPORT_LIB}")
        endif()
    elseif(APPLE)
        target_link_options(obs-android-webcamera PRIVATE -undefined dynamic_lookup)
    endif()
endif()

# 2. Locate FFmpeg libraries (libavformat, libavcodec, libavutil, libswscale)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT MSVC)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat
        libavcodec
        libavutil
        libswscale
    )
    set(AWC_FFMPEG_TARGET PkgConfig::FFMPEG)
else()
    find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h
        HINTS "${FFMPEG_ROOT}/include" "$ENV{FFMPEG_ROOT}/include"
    )
    find_library(AVFORMAT_LIBRARY NAMES avformat libavformat HINTS "${FFMPEG_ROOT}/lib" "$ENV{FFMPEG_ROOT}/lib")
    find_library(AVCODEC_LIBRARY NAMES avcodec libavcodec HINTS "${FFMPEG_ROOT}/lib" "$ENV{FFMPEG_ROOT}/lib")
    find_library(AVUTIL_LIBRARY NAMES avutil libavutil HINTS "${FFMPEG_ROOT}/lib" "$ENV{FFMPEG_ROOT}/lib")
    find_library(SWSCALE_LIBRARY NAMES swscale libswscale HINTS "${FFMPEG_ROOT}/lib" "$ENV{FFMPEG_ROOT}/lib")

    if(NOT FFMPEG_INCLUDE_DIR OR NOT AVFORMAT_LIBRARY OR NOT AVCODEC_LIBRARY OR NOT AVUTIL_LIBRARY OR NOT SWSCALE_LIBRARY)
        message(FATAL_ERROR "FFmpeg development headers/libraries (avformat, avcodec, avutil, swscale) not found. Set FFMPEG_ROOT or CMAKE_PREFIX_PATH.")
    endif()

    add_library(awc_ffmpeg INTERFACE)
    target_include_directories(awc_ffmpeg INTERFACE "${FFMPEG_INCLUDE_DIR}")
    target_link_libraries(awc_ffmpeg INTERFACE
        "${AVFORMAT_LIBRARY}"
        "${AVCODEC_LIBRARY}"
        "${AVUTIL_LIBRARY}"
        "${SWSCALE_LIBRARY}"
    )
    set(AWC_FFMPEG_TARGET awc_ffmpeg)
endif()

target_link_libraries(obs-android-webcamera PRIVATE ${AWC_FFMPEG_TARGET})

if(WIN32)
    target_link_libraries(obs-android-webcamera PRIVATE ws2_32)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(obs-android-webcamera PRIVATE pthread)
endif()

# Copy locale data next to the built plugin binary
add_custom_command(TARGET obs-android-webcamera POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/data"
        "$<TARGET_FILE_DIR:obs-android-webcamera>/data"
)

if(BUILD_TESTS)
    enable_testing()
    add_executable(awc-obs-tests
        ${AWC_CORE_SOURCES}
        src/plugin-main.cpp
        tests/mock_obs.cpp
        tests/test_main.cpp
    )
    target_include_directories(awc-obs-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include/obs-compat"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests"
    )
    target_compile_definitions(awc-obs-tests PRIVATE AWC_STATIC_MOCK_OBS)
    if(MSVC)
        target_compile_options(awc-obs-tests PRIVATE /W4 /utf-8)
        target_compile_definitions(awc-obs-tests PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    else()
        target_compile_options(awc-obs-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    target_link_libraries(awc-obs-tests PRIVATE ${AWC_FFMPEG_TARGET})
    if(WIN32)
        target_link_libraries(awc-obs-tests PRIVATE ws2_32)
    else()
        target_link_libraries(awc-obs-tests PRIVATE pthread)
    endif()
    add_test(NAME awc-obs-tests COMMAND awc-obs-tests)
endif()
