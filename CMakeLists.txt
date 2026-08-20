cmake_minimum_required(VERSION 3.25)
project(SteamHapticsOutput VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(SAMPLERATE REQUIRED IMPORTED_TARGET samplerate)

function(sho_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endfunction()

add_library(sho-core STATIC src/Core.cpp)
target_include_directories(sho-core PUBLIC include)
target_link_libraries(sho-core PUBLIC PkgConfig::SAMPLERATE Threads::Threads)
sho_warnings(sho-core)

if(BUILD_TESTING)
  add_executable(sho-tests tests/tests.cpp)
  target_link_libraries(sho-tests PRIVATE sho-core)
  sho_warnings(sho-tests)
  add_test(NAME sho-tests COMMAND sho-tests)
endif()
