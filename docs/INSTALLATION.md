# Installation

## Requirements

- A POSIX system. Tested on Linux and macOS; the BSDs should work but are
  not tested. On Windows, use WSL.
- A C99 compiler (gcc or clang)
- [CMake (3.10 or higher)](https://cmake.org/download/)

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

3. Optionally, run the test suite and install it system-wide:

    ```bash
    ctest                 # run the conformance tests
    sudo make install     # installs to /usr/local/bin/cpsh
    ```

    To use it as your login shell, add its path to `/etc/shells` and run
    `chsh -s /usr/local/bin/cpsh`.

### Development builds

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DCPSH_SANITIZE=ON   # ASan + UBSan
make && ctest
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
