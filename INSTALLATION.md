# Installation

## Requirements
- A Unix-like operating system: macOS, Linux, BSD. On Windows, WSL is recommended.
- Windows does not fully support POSIX APIs, therefore for now, it will not compile on Windows. (looking for a fix)
- A gcc compiler
- [CMake](https://cmake.org/download/)

## How to run
1. Clone the repository
2. Run the following commands:
```bash
cd path/to/this/repo
make
./shell
```
3. To clean up, run:
```bash
make clean
```
