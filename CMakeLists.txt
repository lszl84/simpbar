cmake_minimum_required(VERSION 3.16)
project(simpbar VERSION 1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(WAYLAND_CURSOR REQUIRED wayland-cursor)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_check_modules(CAIRO REQUIRED cairo)
pkg_check_modules(RSVG REQUIRED librsvg-2.0)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
pkg_check_modules(UDEV REQUIRED libudev)

set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")

file(GLOB PROTOCOL_FILES
    "${CMAKE_SOURCE_DIR}/protocol/unstable/*.xml"
    "${CMAKE_SOURCE_DIR}/protocol/stable/*.xml"
)

set(PROTOCOL_SOURCES "")
set(PROTOCOL_HDRS "")
foreach(protocol ${PROTOCOL_FILES})
    get_filename_component(protocol_name ${protocol} NAME_WE)

    add_custom_command(
        OUTPUT "${GENERATED_DIR}/${protocol_name}-client-protocol.h"
        COMMAND mkdir -p ${GENERATED_DIR} && wayland-scanner client-header ${protocol} ${GENERATED_DIR}/${protocol_name}-client-protocol.h
        DEPENDS ${protocol}
        COMMENT "Generating ${protocol_name} client header"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${GENERATED_DIR}/${protocol_name}-protocol.c"
        COMMAND mkdir -p ${GENERATED_DIR} && wayland-scanner private-code ${protocol} ${GENERATED_DIR}/${protocol_name}-protocol.c
        DEPENDS ${protocol}
        COMMENT "Generating ${protocol_name} private code"
        VERBATIM
    )

    list(APPEND PROTOCOL_HDRS "${GENERATED_DIR}/${protocol_name}-client-protocol.h")
    list(APPEND PROTOCOL_SOURCES "${GENERATED_DIR}/${protocol_name}-protocol.c")
endforeach()

add_custom_target(protocols ALL DEPENDS ${PROTOCOL_HDRS} ${PROTOCOL_SOURCES})

add_executable(simpbar
    src/main.c
    src/bar.c
    src/registry.c
    src/workspace.c
    src/toplevel.c
    src/icon_loader.c
    src/battery.c
    src/battery_history.c
    src/popup.c
    src/volume.c
    src/backlight.c
    src/clock.c
    ${PROTOCOL_SOURCES}
)

add_dependencies(simpbar protocols)

target_include_directories(simpbar PRIVATE
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${WAYLAND_CURSOR_INCLUDE_DIRS}
    ${CAIRO_INCLUDE_DIRS}
    ${RSVG_INCLUDE_DIRS}
    ${PIPEWIRE_INCLUDE_DIRS}
    ${UDEV_INCLUDE_DIRS}
    ${GENERATED_DIR}
)

target_link_libraries(simpbar
    ${WAYLAND_CLIENT_LIBRARIES}
    ${WAYLAND_CURSOR_LIBRARIES}
    ${CAIRO_LIBRARIES}
    ${RSVG_LIBRARIES}
    ${PIPEWIRE_LIBRARIES}
    ${UDEV_LIBRARIES}
    m
    rt
)

# Release build flags
target_compile_options(simpbar PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)

# Install
include(GNUInstallDirs)

install(TARGETS simpbar
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY protocol/
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/simpbar/protocol
    FILES_MATCHING PATTERN "*.xml"
)

# Uninstall target
add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -E remove ${CMAKE_INSTALL_FULL_BINDIR}/simpbar
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_INSTALL_FULL_DATAROOTDIR}/simpbar/protocol
    COMMENT "Uninstalling simpbar"
)