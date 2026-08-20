cmake_minimum_required(VERSION 3.25)
project(SteamHapticsOutput VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(SAMPLERATE REQUIRED IMPORTED_TARGET samplerate)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "This first milestone implements the Linux PipeWire backend only")
endif()
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

function(sho_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endfunction()

add_library(sho-core STATIC src/Core.cpp)
target_include_directories(sho-core PUBLIC include)
target_link_libraries(sho-core PUBLIC PkgConfig::SAMPLERATE Threads::Threads)
sho_warnings(sho-core)

add_library(sho-platform-pipewire STATIC src/PipeWireCapture.cpp)
target_include_directories(sho-platform-pipewire PUBLIC include)
target_link_libraries(sho-platform-pipewire PUBLIC sho-core PRIVATE PkgConfig::PIPEWIRE)
sho_warnings(sho-platform-pipewire)

if(BUILD_TESTING)
  add_executable(sho-tests tests/tests.cpp)
  target_link_libraries(sho-tests PRIVATE sho-core)
  sho_warnings(sho-tests)
  add_test(NAME sho-tests COMMAND sho-tests)
endif()
