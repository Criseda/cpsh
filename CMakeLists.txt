cmake_minimum_required(VERSION 3.10)
project(cpsh C)

# Set the output directory for the binary
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/../bin)

# Include the header files directory
include_directories(include)

# Specify the C standard
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED True)

# Gather all source files
file(GLOB SOURCES "src/*.c")

# Add the executable
add_executable(cpsh ${SOURCES})

# Uninstall target
add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -E echo "Setting uninstall script as executable..."
    COMMAND ${CMAKE_COMMAND} -E env chmod +x ${CMAKE_SOURCE_DIR}/uninstall.sh
    COMMAND ${CMAKE_COMMAND} -E echo "Running uninstall script..."
    COMMAND ${CMAKE_COMMAND} -E env bash ${CMAKE_SOURCE_DIR}/uninstall.sh
)