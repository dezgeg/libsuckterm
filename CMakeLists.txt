cmake_minimum_required(VERSION 2.8.4)
project(libsuckterm)

add_definitions(-DVERSION="0.4.1")
set(CMAKE_C_FLAGS "${CMAKE_CXX_FLAGS} -std=c99 -g3 -pedantic -Wall -Wvariadic-macros -D_BSD_SOURCE -D_XOPEN_SOURCE=600")
find_package(PkgConfig REQUIRED)
pkg_check_modules(PC_FONTCONFIG QUIET fontconfig)
pkg_check_modules(PC_FREETYPE2 QUIET freetype2)
pkg_check_modules(PC_X11 QUIET x11)
pkg_check_modules(PC_XFT QUIET xft)

set(SOURCE_FILES
    libsuckterm.h
    st.c
    arg.h
    config.h
    helpers.h
    helpers.c
    ptyutils.h
    ptyutils.c
    xgui.c)

include_directories(${PC_FONTCONFIG_INCLUDE_DIRS})
link_directories(${PC_FONTCONFIG_LIBRARY_DIRS})
add_definitions(${PC_FONTCONFIG_CFLAGS_OTHER})
include_directories(${PC_FREETYPE2_INCLUDE_DIRS})
link_directories(${PC_FREETYPE2_LIBRARY_DIRS})
add_definitions(${PC_FREETYPE2_CFLAGS_OTHER})
include_directories(${PC_X11_INCLUDE_DIRS})
link_directories(${PC_X11_LIBRARY_DIRS})
add_definitions(${PC_X11_CFLAGS_OTHER})
include_directories(${PC_XFT_INCLUDE_DIRS})
link_directories(${PC_XFT_LIBRARY_DIRS})
add_definitions(${PC_XFT_CFLAGS_OTHER})

add_executable(st ${SOURCE_FILES})
target_link_libraries(st ${PC_FONTCONFIG_LIBRARIES})
target_link_libraries(st ${PC_FREETYPE2_LIBRARIES})
target_link_libraries(st ${PC_X11_LIBRARIES})
target_link_libraries(st ${PC_XFT_LIBRARIES})
target_link_libraries(st "-lutil")
