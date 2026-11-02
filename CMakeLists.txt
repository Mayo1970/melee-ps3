cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported; use a separate binary directory")
endif()

project(melee LANGUAGES C)

# The game's DWARF for the native .dat schema (ps3/tools/datgen/datgen.sh); the only build left here.
option(MELEE_DWARF "Emit DWARF describing the original type layouts" OFF)
if(NOT MELEE_DWARF)
    message(FATAL_ERROR "Only the DWARF build is supported: configure with -DMELEE_DWARF=ON")
endif()

include(cmake/MeleeConfig.cmake)
include(cmake/Dwarf.cmake)
