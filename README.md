# shadNet
Custom online server for shadPS4

Based on RPCSN implementation, but in C++. If anyone wonders why QT, it's because it has all the necessary components on box.

## Building
### Prerequisites

| Requirement | Notes                                                         |
|-------------|---------------------------------------------------------------|
| CMake | >= 3.16                                                       |
| C++ compiler | C++17 capable. Clang 19, GCC, or MSVC 2022 / clang-cl         |
| Ninja | Recommended generator.                                        |
| Qt6 | Components: **Core, Network, Sql, Concurrent, HttpServer**.   |
| Git | Needed to fetch the external submodules                       |

The SQLite Qt SQL driver (`qsqlite`) must be available at runtime for the
database layer.

### 1. Clone with submodules

The `externals/protobuf` submodule is required — the build will not configure
without it.

```bash
git clone --recursive https://github.com/<owner>/shadNet.git
cd shadNet
# already cloned without --recursive?
git submodule update --init --recursive
```

### 2. Install Qt6
**Linux (Ubuntu):**

```bash
sudo apt-get update
# Add LLVM repository
wget -qO - https://apt.llvm.org/llvm-snapshot.gpg.key | sudo apt-key add -
sudo add-apt-repository 'deb http://apt.llvm.org/noble/ llvm-toolchain-noble-19 main'
# Install dependencies
sudo apt-get install -y ninja-build mold clang-19 qt6-base-dev libqt6sql6-sqlite
sudo apt install qt6-base qt6-httpserver qt6-websockets
```

**Windows:** install Qt6 for `win64_msvc2022_64` (with the `qthttpserver` and
`qtwebsockets` modules) plus Visual Studio 2022, and set
`QTDIR`/`CMAKE_PREFIX_PATH` to the Qt kit directory.

### 3. Configure & build
```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

### 4. Run

```bash
./build/shadnet
```