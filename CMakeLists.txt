# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

cmake_minimum_required(VERSION 3.25...3.31)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

include(cmake/validate_special_target.cmake)
include(cmake/version.cmake)
desktop_app_parse_version(Telegram/build/version)

if (NOT DEFINED CMAKE_CONFIGURATION_TYPES)
    set(configuration_types_init 1)
endif()

project(Telegram
    LANGUAGES C CXX
    VERSION ${desktop_app_version_cmake}
    DESCRIPTION "Official Telegram Desktop messenger"
    HOMEPAGE_URL "https://desktop.telegram.org"
)

if (APPLE)
    enable_language(OBJC OBJCXX)
endif()

if (configuration_types_init
    AND CMAKE_CONFIGURATION_TYPES
    AND NOT MinSizeRel IN_LIST CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES};MinSizeRel" CACHE STRING "" FORCE)
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Telegram)

get_filename_component(third_party_loc "Telegram/ThirdParty" REALPATH)
get_filename_component(submodules_loc "Telegram" REALPATH)
get_filename_component(cmake_helpers_loc "cmake" REALPATH)

if (NOT DESKTOP_APP_USE_PACKAGED AND WIN32)
    set(Python3_EXECUTABLE ${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/python/Scripts/python)
endif()

include(cmake/variables.cmake)
include(cmake/nice_target_sources.cmake)
include(cmake/target_compile_options_if_exists.cmake)
include(cmake/target_link_frameworks.cmake)
include(cmake/target_link_options_if_exists.cmake)
include(cmake/init_target.cmake)
include(cmake/generate_target.cmake)
include(cmake/nuget.cmake)
include(cmake/validate_d3d_compiler.cmake)
include(cmake/target_prepare_qrc.cmake)

include(cmake/options.cmake)
include(cmake/external/qt/package.cmake)
if (WIN32 AND NOT DESKTOP_APP_USE_PACKAGED)
    # Local zlib uses different file names than upstream expects.
    set(ZLIB_LIBRARY_DEBUG ${libs_loc}/zlib/Debug/libzsd.lib)
    set(ZLIB_LIBRARY_RELEASE ${libs_loc}/zlib/Release/libzs.lib)
    # Prebuilt Qt recorded the upstream names in its own link interface,
    # and it resolves them before the lines above run. Point them at the
    # real files instead. Runs on every configure, nothing to maintain.
    foreach(qt_lib Core Gui Widgets Network Svg OpenGL OpenGLWidgets Sql)
        if (TARGET Qt6::${qt_lib})
            get_target_property(qt_iface Qt6::${qt_lib} INTERFACE_LINK_LIBRARIES)
            if (qt_iface)
                string(REPLACE
                    "zlib/Debug/zlibstaticd.lib"
                    "zlib/Debug/libzsd.lib"
                    qt_iface
                    "${qt_iface}")
                string(REPLACE
                    "zlib/Release/zlibstatic.lib"
                    "zlib/Release/libzs.lib"
                    qt_iface
                    "${qt_iface}")
                set_target_properties(Qt6::${qt_lib} PROPERTIES
                    INTERFACE_LINK_LIBRARIES "${qt_iface}")
            endif()
        endif()
    endforeach()
    # Qt's own zlib reference is a named target resolved while the shared
    # include above still holds the upstream names. Repair it in place.
    if (TARGET ZLIB::ZLIB)
        set_target_properties(ZLIB::ZLIB PROPERTIES
            IMPORTED_LOCATION_DEBUG ${libs_loc}/zlib/Debug/libzsd.lib
            IMPORTED_LOCATION_RELEASE ${libs_loc}/zlib/Release/libzs.lib)
    endif()
endif()
if (WIN32
    AND NOT DESKTOP_APP_USE_PACKAGED
    AND CMAKE_MSVC_DEBUG_INFORMATION_FORMAT STREQUAL "Embedded")
    # Daily builds: drop the /DEBUG that common options force on every
    # Debug link, so the linker writes no PDB. Crash builds
    # (ProgramDatabase) keep it. Reapplied on every configure.
    get_target_property(common_iface common_options INTERFACE_LINK_OPTIONS)
    if (common_iface)
        set(common_fixed "")
        foreach(opt IN LISTS common_iface)
            if (NOT opt MATCHES ":/DEBUG>$")
                list(APPEND common_fixed "${opt}")
            endif()
        endforeach()
        set_target_properties(common_options PROPERTIES
            INTERFACE_LINK_OPTIONS "${common_fixed}")
    endif()
endif()

set(desktop_app_skip_libs
    glibmm
    variant
)

set(MSVC_DEBUG_INFORMATION_FORMAT "OldStyle" CACHE STRING "MSVC debug information format" FORCE)

add_subdirectory(cmake)
if (WIN32 AND NOT DESKTOP_APP_USE_PACKAGED)
    # Our FFmpeg is built with the MSVC toolchain (.lib), not MinGW (.a).
    set_property(TARGET external_ffmpeg PROPERTY INTERFACE_LINK_LIBRARIES
        ${libs_loc}/ffmpeg/libavfilter/avfilter.lib
        ${libs_loc}/ffmpeg/libavformat/avformat.lib
        ${libs_loc}/ffmpeg/libavcodec/avcodec.lib
        ${libs_loc}/ffmpeg/libswresample/swresample.lib
        ${libs_loc}/ffmpeg/libswscale/swscale.lib
        ${libs_loc}/ffmpeg/libavutil/avutil.lib
        $<LINK_ONLY:desktop-app::external_openh264>
        $<TARGET_FILE:desktop-app::external_openh264>
        $<LINK_ONLY:desktop-app::external_opus>
        $<TARGET_FILE:desktop-app::external_opus>
        $<LINK_ONLY:desktop-app::external_vpx>
        $<TARGET_FILE:desktop-app::external_vpx>)
endif()
add_subdirectory(Telegram)
