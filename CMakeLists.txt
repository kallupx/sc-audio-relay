cmake_minimum_required(VERSION 3.25)
project(SCAudioRelay VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
include(CTest)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(HIDAPI REQUIRED IMPORTED_TARGET hidapi-hidraw)
pkg_check_modules(SAMPLERATE REQUIRED IMPORTED_TARGET samplerate)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "This first milestone implements the Linux PipeWire backend only")
endif()
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

FetchContent_Declare(
  TritonLib
  GIT_REPOSITORY https://github.com/Pixel1011/TritonLib.git
  GIT_TAG d470e0d3bb6309505a201561278b72d1a9dc16f2
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(TritonLib)

add_library(tritonlib STATIC
  ${tritonlib_SOURCE_DIR}/src/TritonController.cpp
  ${tritonlib_SOURCE_DIR}/src/TritonFinder.cpp
)
target_include_directories(tritonlib SYSTEM PUBLIC ${tritonlib_SOURCE_DIR}/include)
target_link_libraries(tritonlib PUBLIC PkgConfig::HIDAPI Threads::Threads)

function(sho_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endfunction()

add_library(sho-core STATIC src/Core.cpp)
target_include_directories(sho-core PUBLIC include)
target_link_libraries(sho-core PUBLIC PkgConfig::SAMPLERATE Threads::Threads)
sho_warnings(sho-core)

add_library(sho-controller-triton STATIC src/TritonTransport.cpp)
target_include_directories(sho-controller-triton PUBLIC include)
target_link_libraries(sho-controller-triton PUBLIC sho-core PRIVATE tritonlib)
sho_warnings(sho-controller-triton)

add_library(sho-platform-pipewire STATIC src/PipeWireCapture.cpp)
target_include_directories(sho-platform-pipewire PUBLIC include)
target_link_libraries(sho-platform-pipewire PUBLIC sho-core PRIVATE PkgConfig::PIPEWIRE)
sho_warnings(sho-platform-pipewire)

add_executable(sc-audio-relay src/main.cpp)
target_link_libraries(sc-audio-relay PRIVATE sho-controller-triton sho-platform-pipewire)
sho_warnings(sc-audio-relay)

if(BUILD_TESTING)
  add_executable(sho-tests tests/tests.cpp)
  target_link_libraries(sho-tests PRIVATE sho-core)
  sho_warnings(sho-tests)
  add_test(NAME sho-tests COMMAND sho-tests)

  add_test(
    NAME sho-cli-source-conflict
    COMMAND sh -c
            "output=\"$('${CMAKE_CURRENT_BINARY_DIR}/sc-audio-relay' --virtual-output --mirror-default 2>&1)\"; status=$?; test $status -ne 0 && case \"$output\" in *\"choose only one audio source\"*) exit 0;; esac; printf '%s\\n' \"$output\"; exit 1"
  )
endif()
