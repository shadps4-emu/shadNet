# shadNet
Custom online server for shadPS4.

Based on RPCSN implementation, but in C++. If anyone wonders why QT, it's because it has all the necessary components out of the box.

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
sudo apt install qt6-base-dev qt6-httpserver-dev qt6-websockets-dev
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

On first start the server writes a `shadnet.cfg` (INI format) next to the
binary and listens on the configured ports (defaults: TCP `31313` for the game
protocol, UDP `31314` for matchmaking/STUN, TCP `31315` for the WebAPI).

## Creating an account

shadNet is a **server**, so there is no web signup page or `curl` endpoint
(the WebAPI only exposes `/status`). Accounts are created by a **client** that
connects over the game protocol and sends a `Create` command. In normal use
that client is shadPS4 itself. To create an account manually, use the bundled
reference client in [`clientsample/`](clientsample/), which exposes a
`register` command.

### Using the sample client

The sample client is a standalone tool (protobuf only, no Qt). Build it
separately from its own directory:

```bash
cd clientsample
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Then register against a running server:

```bash
./build/shadnet-sample <host> <port> register <npid> <password> <email> [secretKey]
```

`<port>` is the server's `UnsecuredPort` (default `31313`). For a server on the
same machine with open registration:

```bash
./build/shadnet-sample 127.0.0.1 31313 register MyName hunter2 me@example.com
```

If the server has a `RegistrationSecretKey` set, append it as the last argument:

```bash
./build/shadnet-sample 127.0.0.1 31313 register MyName hunter2 me@example.com MySecret
```

The tool prints the connection result and the server's reply; on success the
account exists and you can `login` with the same client (or from shadPS4).

The fields a registration supplies:

- **npid**: Your NP ID / username (validated; must be unique, case-insensitive)
- **password**: Your passwerd
- **email**: Your email (must be unique)
- **secretKey**: only required if the server operator set one (see below)

### Server-side: controlling who can register

Registration is governed by `shadnet.cfg`:

| Key | Effect                                                                                                                                                |
|-----|-------------------------------------------------------------------------------------------------------------------------------------------------------|
| `RegistrationSecretKey` | Empty (default), then registration is **open** to anyone. Set to a value, then clients must send a matching `secret_key`, or they get `Unauthorized`. |
| `EmailValidated` | When `true`, login requires a validated email token.                                                                                                  |
| Banned domains | Email addresses on a banned domain are rejected (`CreationBannedEmailProvider`).                                                                      |

To host a private instance, set a `RegistrationSecretKey` in `shadnet.cfg` and
share that key only with the people you want to allow to register.