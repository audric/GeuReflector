##############################################################################
# GeuReflector — SvxReflector with server-to-server trunk support
#
# Based on SvxLink by Tobias Blomberg / SM0SVX
# Trunk extension by IW1GEU
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
##############################################################################

cmake_minimum_required(VERSION 3.7...3.30)
project(geureflector C CXX)

set(PROJECT_INCLUDE_DIR ${PROJECT_BINARY_DIR}/include)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
set(CMAKE_BUILD_RPATH "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
set(RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/Modules/")

option(BUILD_STATIC_LIBS "Build static libraries in addition to dynamic" OFF)

if(NOT DEFINED INTERNAL_SAMPLE_RATE)
  set(INTERNAL_SAMPLE_RATE 16000)
endif()
add_definitions(-DINTERNAL_SAMPLE_RATE=${INTERNAL_SAMPLE_RATE})

include_directories(${PROJECT_INCLUDE_DIR} ${CMAKE_BINARY_DIR})

if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX)
  set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -Wall -Wstrict-prototypes -Wpointer-arith")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wpointer-arith")
  set(CMAKE_C_FLAGS_RELEASE   "-O3")
  set(CMAKE_CXX_FLAGS_RELEASE "-O3")
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

##############################################################################
# Install dirs
##############################################################################
include(GNUInstallDirs)

set(BIN_INSTALL_DIR   ${CMAKE_INSTALL_FULL_BINDIR})
set(LIB_INSTALL_DIR   ${CMAKE_INSTALL_FULL_LIBDIR})
set(SYSCONF_INSTALL_DIR ${CMAKE_INSTALL_FULL_SYSCONFDIR})
set(SHARE_INSTALL_PREFIX ${CMAKE_INSTALL_FULL_DATADIR})
set(SVX_SYSCONF_INSTALL_DIR ${SYSCONF_INSTALL_DIR}/svxlink)
set(SVX_SHARE_INSTALL_DIR      ${SHARE_INSTALL_PREFIX}/svxlink)
set(SVX_INCLUDE_INSTALL_DIR    ${CMAKE_INSTALL_FULL_INCLUDEDIR}/svxlink)
set(SVX_MODULE_INSTALL_DIR     ${LIB_INSTALL_DIR}/svxlink)
set(SVX_LOGIC_CORE_INSTALL_DIR ${LIB_INSTALL_DIR}/svxlink)
set(INCLUDE_INSTALL_DIR        ${CMAKE_INSTALL_FULL_INCLUDEDIR})
set(MAN_INSTALL_DIR            ${CMAKE_INSTALL_FULL_MANDIR})
set(DOC_INSTALL_DIR            ${CMAKE_INSTALL_FULL_DOCDIR})
set(SVX_SPOOL_INSTALL_DIR      ${LOCAL_STATE_DIR}/spool/svxlink)

if(NOT DEFINED LOCAL_STATE_DIR)
  set(LOCAL_STATE_DIR ${CMAKE_INSTALL_FULL_LOCALSTATEDIR})
endif()
set(SVX_LOCAL_STATE_DIR ${LOCAL_STATE_DIR}/lib/svxlink)

set(CONFIG_FILES "" CACHE INTERNAL "CONFIG_FILES")
set(FILE_OWNER   "" CACHE INTERNAL "FILE_OWNER")

function(expinc filename)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${filename}
    ${PROJECT_INCLUDE_DIR}/${filename} COPYONLY)
endfunction()

function(install_if_not_exists src dest)
  if(NOT IS_ABSOLUTE "${src}")
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/${src}")
  endif()
  get_filename_component(src_name "${src}" NAME)
  if(NOT IS_ABSOLUTE "${dest}")
    set(dest "${CMAKE_INSTALL_PREFIX}/${dest}")
  endif()
  install(CODE "
    if(NOT EXISTS \"\$ENV{DESTDIR}${dest}/${src_name}\")
      message(STATUS \"Installing: \$ENV{DESTDIR}${dest}/${src_name}\")
      execute_process(COMMAND \${CMAKE_COMMAND} -E copy \"${src}\"
                      \"\$ENV{DESTDIR}${dest}/${src_name}\"
                      RESULT_VARIABLE copy_result ERROR_VARIABLE error_output)
      if(copy_result)
        message(FATAL_ERROR \${error_output})
      endif()
    else()
      message(STATUS \"Skipping  : \$ENV{DESTDIR}${dest}/${src_name}\")
    endif()
  ")
endfunction()

##############################################################################
# Dependencies
##############################################################################
add_subdirectory(include)

configure_file(${CMAKE_SOURCE_DIR}/config.h.in ${CMAKE_BINARY_DIR}/config.h @ONLY)

find_package(SIGC REQUIRED)
include_directories(${SIGC_INCLUDE_DIRS})
add_definitions(${SIGC_DEFINITIONS})
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SIGC_CXX_FLAGS}")
set(LIBS ${LIBS} ${SIGC_LIBRARIES})

include(FindCHOWN)

set(SVXLINK_USER  "svxlink" CACHE STRING "SvxLink system user")
set(SVXLINK_GROUP "svxlink" CACHE STRING "SvxLink system group")

##############################################################################
# Subdirectories
##############################################################################
add_subdirectory(async)
add_subdirectory(misc)
add_subdirectory(svxlink)
