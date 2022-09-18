cmake_minimum_required(VERSION 3.21)
project(evio C)

option(BUILD_SHARED_LIBS "Build shared libraries" OFF)

add_subdirectory(src)

find_program(ASTYLE astyle)
if(ASTYLE)
    add_custom_target(astyle
        VERBATIM
        USES_TERMINAL
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMAND ${ASTYLE} --options=${CMAKE_SOURCE_DIR}/.astylerc -Q -n -r
            "src/*.c" "src/*.h"
    )
endif()
