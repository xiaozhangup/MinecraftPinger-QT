cmake_minimum_required(VERSION 3.16)

project(MinecraftPinger LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Prefer Qt6, fallback to Qt5.
find_package(Qt6 COMPONENTS Widgets QUIET)
if (Qt6_FOUND)
  set(QT_VERSION_MAJOR 6)
  set(QT_LIB_PREFIX Qt6)
  find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
else()
  find_package(Qt5 REQUIRED COMPONENTS Widgets Network)
  set(QT_VERSION_MAJOR 5)
  set(QT_LIB_PREFIX Qt5)
endif()

add_executable(MinecraftPinger
  src/main.cpp
  src/MainWindow.h
  src/MainWindow.cpp
)

target_link_libraries(MinecraftPinger PRIVATE ${QT_LIB_PREFIX}::Widgets ${QT_LIB_PREFIX}::Network)

# Optional: On X11 we can set an input-only shape so the shadow area becomes click-through.
if (UNIX AND NOT APPLE)
  find_package(X11 QUIET)
  if (X11_FOUND)
    # XShape is provided by Xext.
    if (TARGET X11::Xext)
      target_link_libraries(MinecraftPinger PRIVATE X11::X11 X11::Xext)
      target_compile_definitions(MinecraftPinger PRIVATE HAVE_X11_SHAPE=1)
    elseif (X11_Xext_FOUND)
      target_link_libraries(MinecraftPinger PRIVATE ${X11_LIBRARIES} ${X11_Xext_LIB})
      target_compile_definitions(MinecraftPinger PRIVATE HAVE_X11_SHAPE=1)
    endif()
  endif()
endif()

# Enable Qt's automatic processing (MOC/UIC/RCC) for convenience.
set_target_properties(MinecraftPinger PROPERTIES
  AUTOMOC ON
  AUTOUIC ON
  AUTORCC ON
)

# Qt6 recommended finalize step.
if (QT_VERSION_MAJOR EQUAL 6)
  qt_finalize_executable(MinecraftPinger)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)