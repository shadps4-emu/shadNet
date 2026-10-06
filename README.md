# shadNet
Custom online server for shadPS4.

Based on RPCSN implementation, but in C++. If anyone wonders why Qt, it's because it has all the necessary components out of the box.

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

The `externals/protobuf` and `externals/abseil-cpp` submodules are required.

```bash
git clone --recursive https://github.com/shadps4-emu/shadNet.git
cd shadNet
# already cloned without --recursive?
git submodule update --init --recursive
```

### 2. Install Qt6
**Linux:** install a Qt6 desktop kit with HTTP Server and WebSockets, using
the Qt installer or `aqtinstall`. Set `QTDIR` to that
kit's directory. The Ubuntu package commands below install build tools;
they do not select a Qt kit.

```bash
sudo apt-get update
sudo apt-get install -y cmake ninja-build g++
# Replace this with your installed Qt kit.
export QTDIR="$HOME/Qt/<version>/gcc_64"
```

**Windows:** install Qt6 for `win64_msvc2022_64` (with the `qthttpserver` and
`qtwebsockets` modules) plus the Visual Studio 2022 C++ build tools. Set
`QTDIR`/`CMAKE_PREFIX_PATH` to the Qt kit directory, for example
`C:\Qt\<version>\msvc2022_64`.

### 3. Configure & build

**Linux:**

```bash
cmake -S . -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QTDIR"
cmake --build build --config Release --parallel
```

**Windows (VS Code with CMake Tools):**

1. Open the repository folder in VS Code and install the **CMake Tools** extension.
2. Run **CMake: Select a Kit** and choose the Visual Studio 2022 **amd64** kit.
   CMake Tools sets up the compiler and Windows SDK; no Developer PowerShell is needed.
3. In workspace settings, set **CMake: Build Directory** to `${workspaceFolder}/build`,
   **CMake: Generator** to `Ninja`, and add `CMAKE_PREFIX_PATH` under
   **CMake: Configure Settings**, pointing to your Qt kit (for example,
   `C:/Qt/<version>/msvc2022_64`).
4. Run **CMake: Select Variant**, choose **Release**, then run **CMake: Configure**
   and **CMake: Build**.

To copy the Qt runtime files, open a regular **Command Prompt** in the repository
folder and run:

```cmd
rem Replace this with your installed Qt kit.
set "QTDIR=C:\Qt\<version>\msvc2022_64"
"%QTDIR%\bin\windeployqt6.exe" --release --no-translations .\build\shadnet.exe
```

Some Qt installations name the deployment tool `windeployqt.exe`. Use the
one supplied with the kit you built against. It copies Qt DLLs and plugins,
including `sqldrivers/qsqlite.dll`, beside the executable. On Linux, keep the
matching Qt shared libraries and SQL driver available through your Qt installation.

### 4. Run

Copy the supplied world and leaderboard definitions next to the executable.

**Windows:** in File Explorer, copy `worlds.cfg` and `scoreboards.cfg` into
the `build` folder, then double-click `shadnet.exe` to start the server.

**Linux:**

```bash
cp worlds.cfg scoreboards.cfg build/
./build/shadnet
```

On first start the server writes a `shadnet.cfg` (INI format) next to the
binary. Configuration and data paths are relative to the executable directory.
Defaults bind to `127.0.0.1`: TCP `31313` for the game protocol, `31315` for
the WebAPI, `31320` for stats, and `31350` for the admin API. UDP signaling
is disabled by default; set `Matching2Enabled=true` to start UDP `31314`.

### Using shadNet Toolbox

The `membertool` directory contains **shadNet Toolbox**, a Qt application for
local server tools, including account management and Worlds configuration.
Open it as a separate CMake project and build `shadnet-toolbox` with your Qt kit
(Core, Sql, Widgets, and Network).
On Windows, copy its Qt runtime files with `windeployqt6.exe` as above.

Stop the server, open the tool, and choose the server's `db/shadnet.db`.
Enter a username and click **Create member**. The tool generates a password,
uses `<username>@shadps4.local` for the email, and writes the account to the
database. Copy the login details before closing. To remove an account,
select it in the list and click **Remove selected member**. Restart the
server when finished.

### Editing and reloading Worlds

In shadNet Toolbox, click **Worlds configuration...** and select the
server's `worlds.cfg`. This works independently of opening an accounts database.
You can also open the editor directly:

```bash
shadnet-toolbox --worlds /path/to/server/worlds.cfg
```

Use **Add**, **Edit**, and **Remove** in the **Worlds** and **Title groups**
tables. The tool generates the config syntax, checks numeric ranges and prevents
duplicate entries. Generated files replace original comments and whitespace.
Hand editing remains available in a text editor.

- **Save only** writes `worlds.cfg` without changing the active configuration.
- **Save & reload worlds** writes and activates the draft without restarting.
- **Reload saved worlds** activates the saved file, leaving unsaved edits in the editor.
- **Refresh open file** reloads the selected file; discarding a draft requires confirmation.

The local editor uses a named pipe on Windows or a Unix-domain socket on Unix,
restricted to the server's OS user and keyed to the selected file's canonical
path. Run the tool under the same OS account as the server. No admin API,
remote address or network credentials are used. When the server is stopped,
the editor can save files and create a new config; reload is unavailable until
the server is started and the editor reads the file again. A running server
with an unavailable local socket blocks direct writes. Account editing still
requires stopping the server as described above; Worlds editing does not.

The server also exposes these Worlds endpoints through its admin API. Requests
use a Bearer session and the configured `X-Admin-Api-Key`. Each route addresses
the server's `worlds.cfg`; requests cannot select arbitrary file paths:

| Method and route | Request / result |
| --- | --- |
| `GET /admin/v1/worlds/config` | Snapshot: `content`, `revision`, `activeContent`, `activeRevision`, `canReload`, `exists`. |
| `PUT /admin/v1/worlds/config` | `{content, revision, reload}` saves, optionally activates, and returns a snapshot. |
| `POST /admin/v1/worlds/reload` | `{revision}` activates that saved revision and returns a snapshot. |

The API rechecks current admin status for each request and audits mutations.
Revisions are SHA-256 file hashes, or `missing`. Stale revisions return `409`;
invalid configuration or occupied-world changes return `422`. Editors preserve
drafts on failure and require a fresh read after conflicts or uncertain results.

The server validates the complete config at startup and before changes. Invalid
startup files now fail startup with a line error instead of partially loading.
Files must be UTF-8 and at most 512 KiB. Saves create `worlds.cfg.bak` and use
atomic replacement. Cooperating editors serialize writes through a file lock;
avoid simultaneous text-editor writes because external editors do not honor it.

Reload swaps configuration maps while retaining rooms, indexes, sessions and
sockets. World-list requests see the updated settings immediately after reload.
Title-group changes apply to subsequent logins because existing sessions retain
their original matching group. Worlds containing rooms cannot be removed or
have their world/server IDs changed during reload. Groups without explicit
worlds retain the fallback world-list behavior. Installing this server update
requires one initial restart; later Worlds changes can be reloaded live.

The parser, file storage and editor components live in `common/worlds`.

### Using the sample client

The sample client is a standalone tool (protobuf only, no Qt). Build it
separately from its own directory:

```bash
cd clientsample
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

On Windows, open `clientsample` in VS Code with CMake Tools or Visual Studio
2022, select an x64 MSVC configuration, and build the sample. Run
`shadnet-sample.exe` through the IDE, supplying the arguments shown below
in its launch configuration. Prefer these IDE workflows for Windows tasks;
use PowerShell only as a last resort, not as the default setup.
The sample client's matchmaking code uses an older command map and is not
compatible with the current server.

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

- **npid**: Your NP ID / username, 3–16 characters; letters, digits, underscore, or hyphen. Unique ignoring case, but login requires the registered casing.
- **password**: Your password
- **email**: Your email (must be unique)
- **secretKey**: only required if the server operator set one (see below)

### Server-side: controlling who can register

Registration is governed by `shadnet.cfg`:

| Key | Effect                                                                                                                                                |
|-----|-------------------------------------------------------------------------------------------------------------------------------------------------------|
| `RegistrationSecretKey` | Empty (default), then registration is **open** to anyone. Set to a value, then clients must send a matching `secret_key`, or they get `Unauthorized`. |
| `EmailValidated` | When `true`, game login requires the stored account token. No email-token delivery flow is implemented.                                                                                                  |
| `domains_banlist.txt` | Binary registration rejects listed email domains (`CreationBannedEmailProvider`); HTTP registration does not apply this list.                                                                      |

To host a private instance, set a `RegistrationSecretKey` in `shadnet.cfg` and
share that key only with the people you want to allow to register.
