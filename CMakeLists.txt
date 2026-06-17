cmake_minimum_required(VERSION 3.21)
project(hyprplane CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
# hyprland.pc pulls in aquamarine/hyprutils/hyprlang/pixman/wayland/etc transitively.
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET
    hyprland
    hyprgraphics
    pixman-1
    libdrm
    pangocairo
    cairo
)

add_library(hyprplane SHARED
    src/main.cpp
    src/CanvasMode.cpp
)

target_compile_options(hyprplane PRIVATE -fPIC -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(hyprplane PRIVATE PkgConfig::DEPS)
set_target_properties(hyprplane PROPERTIES PREFIX "")  # -> hyprplane.so

install(TARGETS hyprplane
    LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/hyprland/plugins)
