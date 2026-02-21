cmake_minimum_required(VERSION 3.16)

project(MinecraftPingerGtk LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(ADW REQUIRED IMPORTED_TARGET libadwaita-1)
pkg_check_modules(JSONGLIB REQUIRED IMPORTED_TARGET json-glib-1.0)
pkg_check_modules(GDKPIXBUF REQUIRED IMPORTED_TARGET gdk-pixbuf-2.0)

add_executable(MinecraftPingerGtk
  src/main.c
  src/pinger-window.c
  src/pinger-window.h
  src/minecraft-ping.c
  src/minecraft-ping.h
  src/progress-ring.c
  src/progress-ring.h
)

target_link_libraries(MinecraftPingerGtk PRIVATE
  PkgConfig::GTK4
  PkgConfig::ADW
  PkgConfig::JSONGLIB
  PkgConfig::GDKPIXBUF
  m
)

# Quiet some deprecation noise across distros, keep warnings reasonable.
target_compile_options(MinecraftPingerGtk PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Helpful for IDEs
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
