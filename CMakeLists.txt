cmake_minimum_required(VERSION 3.10)
project(cpsh VERSION 1.0.0 LANGUAGES C)

# Set the output directory for the binary
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/../bin)

# Optimised build unless asked otherwise
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Specify the C standard
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED True)
set(CMAKE_C_EXTENSIONS OFF)

option(CPSH_SANITIZE "Build with AddressSanitizer and UBSan" OFF)

# Gather all source files
file(GLOB SOURCES "src/*.c")

# Add the executable
add_executable(cpsh ${SOURCES})
target_include_directories(cpsh PRIVATE include)
target_compile_definitions(cpsh PRIVATE _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700)
if(APPLE)
  # keep TIOCGWINSZ and friends visible alongside the POSIX feature macros
  target_compile_definitions(cpsh PRIVATE _DARWIN_C_SOURCE)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(cpsh PRIVATE -Wall -Wextra -Wno-unused-parameter)
  if(CPSH_SANITIZE)
    target_compile_options(cpsh PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(cpsh PRIVATE -fsanitize=address,undefined)
  endif()
endif()

install(TARGETS cpsh RUNTIME DESTINATION bin)

# Tests: ctest (or `make test`) runs the conformance suite against the build
enable_testing()
add_test(NAME conformance
         COMMAND sh ${CMAKE_SOURCE_DIR}/tests/run_tests.sh $<TARGET_FILE:cpsh>)

# Uninstall target
add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -E echo "Setting uninstall script as executable..."
    COMMAND ${CMAKE_COMMAND} -E env chmod +x ${CMAKE_SOURCE_DIR}/uninstall.sh
    COMMAND ${CMAKE_COMMAND} -E echo "Running uninstall script..."
    COMMAND ${CMAKE_COMMAND} -E env bash ${CMAKE_SOURCE_DIR}/uninstall.sh
)
