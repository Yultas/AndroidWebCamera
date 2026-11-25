cmake_minimum_required(VERSION 3.22)
project(awc-win-vcam VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AWC_VCAM_FILTER_ONLY "Build only the DirectShow virtual camera filter DLL (no FFmpeg needed)" OFF)
option(BUILD_TESTS "Build unit and E2E tests" ON)

set(AWC_OBS_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../obs-plugin/src")

# ----------------------------------------------------------------------------
# 1. DirectShow Virtual Camera Filter DLL (Windows x64 / Win32)
# ----------------------------------------------------------------------------
if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(AWC_VCAM_DLL_NAME "awc-vcam-filter64")
    else()
        set(AWC_VCAM_DLL_NAME "awc-vcam-filter32")
    endif()

    add_library(awc-vcam-filter SHARED
        src/vcam-shm.cpp
        src/vcam-convert.cpp
        src/dshow-filter.cpp
        src/dshow-dll.cpp
        src/dshow-filter.def
    )
    set_target_properties(awc-vcam-filter PROPERTIES
        OUTPUT_NAME "${AWC_VCAM_DLL_NAME}"
        PREFIX ""
    )
    target_include_directories(awc-vcam-filter PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(awc-vcam-filter PRIVATE
        strmiids
        ole32
        oleaut32
        advapi32
        winmm
        uuid
    )
    if(MSVC)
        target_compile_options(awc-vcam-filter PRIVATE /W4 /utf-8)
        target_compile_definitions(awc-vcam-filter PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX UNICODE _UNICODE)
    else()
        target_compile_options(awc-vcam-filter PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(AWC_VCAM_FILTER_ONLY)
    return()
endif()

# ----------------------------------------------------------------------------
# 2. Locate FFmpeg (libavformat, libavcodec, libavutil, libswscale)
# ----------------------------------------------------------------------------
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
        message(FATAL_ERROR "FFmpeg development headers/libraries not found. Set FFMPEG_ROOT or pass -DAWC_VCAM_FILTER_ONLY=ON.")
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

# ----------------------------------------------------------------------------
# 3. Autonomous Virtual Camera Host CLI (awc-vcam)
# ----------------------------------------------------------------------------
add_executable(awc-vcam
    src/vcam-host.cpp
    src/vcam-decoder.cpp
    src/vcam-shm.cpp
    "${AWC_OBS_SRC_DIR}/awc-adb.cpp"
    "${AWC_OBS_SRC_DIR}/awc-client.cpp"
    "${AWC_OBS_SRC_DIR}/awc-decoder.cpp"
)
target_include_directories(awc-vcam PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${AWC_OBS_SRC_DIR}"
)
target_link_libraries(awc-vcam PRIVATE ${AWC_FFMPEG_TARGET})
if(WIN32)
    target_link_libraries(awc-vcam PRIVATE ws2_32)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(awc-vcam PRIVATE pthread)
endif()
if(MSVC)
    target_compile_options(awc-vcam PRIVATE /W4 /utf-8)
    target_compile_definitions(awc-vcam PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(awc-vcam PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ----------------------------------------------------------------------------
# 4. Unit & FFmpeg E2E Tests
# ----------------------------------------------------------------------------
if(BUILD_TESTS)
    enable_testing()
    add_executable(test_vcam
        tests/test_vcam.cpp
        src/vcam-decoder.cpp
        src/vcam-shm.cpp
        src/vcam-convert.cpp
        "${AWC_OBS_SRC_DIR}/awc-client.cpp"
        "${AWC_OBS_SRC_DIR}/awc-decoder.cpp"
    )
    target_include_directories(test_vcam PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${AWC_OBS_SRC_DIR}"
    )
    target_link_libraries(test_vcam PRIVATE ${AWC_FFMPEG_TARGET})
    if(WIN32)
        target_link_libraries(test_vcam PRIVATE ws2_32)
    elseif(UNIX AND NOT APPLE)
        target_link_libraries(test_vcam PRIVATE pthread)
    endif()
    if(MSVC)
        target_compile_options(test_vcam PRIVATE /W4 /utf-8)
        target_compile_definitions(test_vcam PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    else()
        target_compile_options(test_vcam PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME win_vcam_unit_and_e2e COMMAND test_vcam)
endif()
