cmake_minimum_required(VERSION 3.20)
project(TidyTray VERSION 1.0.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(TidyTray WIN32
  src/main.cpp
  src/Rules.cpp
  src/Icons.cpp
  src/Ui.cpp
  src/resource.rc
)

set_source_files_properties(src/resource.rc PROPERTIES
  COMPILE_FLAGS "-I${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(TidyTray PRIVATE
  UNICODE
  _UNICODE
  _WIN32_WINNT=0x0A00
  WINVER=0x0A00
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)

target_compile_options(TidyTray PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -finput-charset=UTF-8
  -fexec-charset=UTF-8
)

target_link_options(TidyTray PRIVATE
  -municode
  -static-libgcc
  -static-libstdc++
  -static
)

find_package(Threads REQUIRED)

target_link_libraries(TidyTray PRIVATE
  Threads::Threads
  user32
  gdi32
  shell32
  shlwapi
  comctl32
  advapi32
  ole32
  oleaut32
  uuid
  windowscodecs
  version
  uxtheme
)
