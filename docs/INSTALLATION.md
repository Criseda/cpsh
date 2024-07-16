# Installation

## Requirements

- A Unix-like operating system: macOS, Linux, BSD. On Windows, WSL is recommended.
- Windows does not fully support POSIX APIs, therefore it will not compile on windows
- A gcc compiler
- [CMake](https://cmake.org/download/)

## How to install and run the shell

1. Clone the repository
2. Run the following commands to build and run the shell:

    ```bash
    cd path/to/this/repo
    mkdir build
    cd build
    cmake ..
    make
    ../bin/cpsh # to run the shell
    ```

## Uninstalling

To remove associated files created in the home directory, follow these steps:

1. Navigate to the directory where you built the project, for example:

    ```bash
    cd path/to/this/repo/build
    ```

2. Run the following command to execute the uninstall target:

    ```bash
    cmake --build . --target uninstall
    ```

    This will run the `uninstall.sh` script, which will remove the history file and any other files specified in the script.
