cmake_minimum_required(VERSION 3.15)
project(sshp C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Werror -Wextra -Wpedantic -O2 -Wno-unused-parameter")

# Detect the operating system
if(${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    add_definitions(-DUSE_KQUEUE=1)
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "FreeBSD")
    add_definitions(-DUSE_KQUEUE=1)
else()
    add_definitions(-DUSE_KQUEUE=0)
endif()

# Source files
set(SOURCES src/sshp.c src/fdwatcher.c)

# Header files
set(HEADERS src/fdwatcher.h)

# Executable target
add_executable(sshp ${SOURCES} ${HEADERS})

# Man page generation
find_program(MD2MAN_ROFF md2man-roff)
if(MD2MAN_ROFF)
    add_custom_command(
        OUTPUT man/sshp.1
        COMMAND ${MD2MAN_ROFF} man/sshp.md > man/sshp.1
        DEPENDS man/sshp.md
        COMMENT "Generating man page"
    )
    add_custom_target(man ALL DEPENDS man/sshp.1)
endif()

# Install target
install(TARGETS sshp DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)
install(FILES man/sshp.1 DESTINATION ${CMAKE_INSTALL_PREFIX}/share/man/man1)
