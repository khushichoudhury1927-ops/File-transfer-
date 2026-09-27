# Secure FTP Server

A high-performance **C++20** file transfer system that provides secure, fault-tolerant file transfers with automatic transfer resumption. The project leverages **OpenSSL** for TLS encryption, ensuring files remain protected while in transit between the client and server.

## Documentation

| Document | Contents |
| --- | --- |
| [docs/PRD.md](docs/PRD.md) | Project Requirements Document: problem, scope, 20 functional and 12 non-functional requirements, limitations, deliverables |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Layers, component responsibilities, concurrency model, data structures, wire protocol, design decisions, security design |
| [docs/UML.md](docs/UML.md) | Class diagram, three sequence diagrams, two state machine diagrams |
| [docs/DEVELOPMENT_PLAN.md](docs/DEVELOPMENT_PLAN.md) | The six project stages mapped to the work done, timeline, demonstration script |
| [docs/TEST_PLAN.md](docs/TEST_PLAN.md) | Test strategy, coverage traceability, defects found and fixed, measured results |

---

## Overview

This project implements a secure client-server file transfer protocol capable of recovering from network interruptions without restarting the transfer. By combining encrypted communication with transfer checkpointing, the system provides reliable file delivery even in unstable network environments.

---

## Features

-  TLS encryption using OpenSSL
-  Certificate verification, including the host name
-  Secure client-server file transfers
-  Automatic transfer resumption after connection loss
- Fault-tolerant transfer mechanism
- File integrity verification using checksums
- Several clients transferring at the same time
- Strict file name validation
- Cross-platform networking with CMake
- Modular and extensible codebase

---

## Tech Stack

- **Language:** C++20
- **Build System:** CMake 3.15+
- **Networking:** Boost.Asio
- **Security:** OpenSSL (TLS/SSL)
- **Concurrency:** C++ Threads
- **Sockets:** BSD/POSIX sockets or WinSock

---

## Project Structure

```text
FTP/
│
├── include/
│   ├── checksum.hpp
│   ├── client.hpp
│   ├── logging.hpp
│   ├── protocol.hpp
│   ├── server.hpp
│   └── serverSession.hpp
│
├── src/
│   ├── main.cpp
│   ├── client.cpp
│   ├── server.cpp
│   ├── serverSession.cpp
│   └── checksum.cpp
│
├── tests/
│   └── run_tests.ps1
│
├── tls_key/
│   ├── server.crt
│   └── server.key
│
├── CMakeLists.txt
└── README.md
```

---

## Core Components

### Server

- Accepts incoming client connections
- Creates a session for every client, all served concurrently
- Coordinates secure file transfers
- Manages encrypted communication
- Lets only one client at a time write a given `received_<name>`

### Client

- Connects securely to the server and verifies its certificate
- Uploads files, resuming interrupted transfers
- Handles encrypted communication

### Server Session

Each connected client is handled independently through a session responsible for:

- Validating the request before anything is stored
- Receiving file chunks
- Managing transfer state
- Recovering interrupted transfers
- Verifying the digest and deleting a file that does not match

Every step of a session is asynchronous and keeps at most one operation in
flight, so a transfer never occupies a worker thread. The server runs its
`io_context` on a small pool of threads, which is what allows several clients to
transfer at the same time.

### Checksum Module

Provides integrity verification to ensure transferred files are received without corruption. SHA-256 is computed incrementally with OpenSSL's `EVP` interface.

---

## Protocol

```text
client                                   server
  |---- uint32 name_len -------------------->|
  |---- name_len bytes filename ------------>|
  |---- 64 bytes hex SHA-256 --------------->|
  |---- uint64 file_size -------------------->|
  |                                          |  the whole request is validated
  |                                          |  before anything is stored
  |<--- 1 byte status (proceed / refused) ---|
  |<--- uint64 resume_offset ----------------|  only when proceeding
  |---- payload, 64 KB chunks --------------->|
  |<--- 1 byte status (verified / failed) ---|
```

All scalars are host byte order and each is sent as a single write, so the
header never needs packing. Uploads are stored as `received_<name>`, which is
what makes resumption possible: the stored file length is the resume offset.

Because the request is validated up front, a refusal is reported as a status
byte instead of a misleading resume offset. A client whose file is already being
uploaded by somebody else is told the destination is **busy** and exits, rather
than writing over the other transfer.

---

## Fault Tolerance

The transfer engine is designed to recover gracefully from unexpected network failures.

### Recovery Workflow

```text
Client
    │
Upload File
    │
Connection Lost
    │
Reconnect
    │
Resume From Last Received Byte
    │
Transfer Complete
```

Instead of retransmitting the entire file, the client resumes from the last successfully acknowledged offset, reducing bandwidth usage and improving reliability.

Because the resume offset is the size of the partially stored file on disk, a
file whose stored copy is larger than the source is treated as stale and
restarted from zero.

---

## Security

All communication between the client and server is encrypted using **TLS**.

Security features include:

- TLS encrypted communication
- OpenSSL integration
- Server certificate
- Private key authentication
- Protected data in transit
- Server name indication (SNI), so the server is asked for by name
- Certificate chain **and** host name verification, on by default
- SHA-256 verified after every transfer; a file that does not match is deleted
- Strict file name validation, so a client cannot write outside the target
  directory or onto a device name

The client verifies the server by default. It looks for `server.crt` next to the
executable, then in `./tls_key`, which is where the build copies the
development certificate. Use `--ca <file>` to verify against a different
authority. `--insecure` turns verification off and prints a warning; it exists
for troubleshooting and should not be used on a real network.

Because the bundled certificate is self-signed, a trusted deployment should
replace it with a certificate issued for the host it runs on.

---

## File Integrity

After a transfer completes, a checksum is computed to verify that the received file matches the original.

Benefits include:

- Corruption detection
- Transmission verification
- Reliable delivery

---

## Building the Project

### Requirements

- CMake 3.15+
- C++20 capable compiler (MSVC 2019+, GCC 10+, Clang 10+)
- Boost 1.70+ (**headers only**, used by Boost.Asio)
- OpenSSL 1.1.1+ development libraries

CMake never links Boost libraries, because Asio is header only. OpenSSL *is*
linked, so both its headers and its import/static libraries are required.

> **Note for MinGW (including the toolchain bundled with CLion):** the bundled
> GCC does not ship OpenSSL or Boost, so install both yourself. `FindOpenSSL` on
> Windows also ignores MinGW's `libssl.a` / `libcrypto.a` archives, so
> `CMakeLists.txt` falls back to a plain header/library search driven by
> `OPENSSL_ROOT_DIR`. When configuring from a plain `cmd`/`PowerShell` window,
> also put CLion's `bin\mingw\bin` on `PATH`, otherwise CMake cannot find
> `mingw32-make.exe` for the generator and reports the compiler as broken.

---

### Clone

```bash
git clone https://github.com/zexxitywave/File-transfer-.git

cd File-transfer-
```

The repository deliberately contains no certificate or private key, so the next
step is required before the server will start.

---

### Generate the TLS certificate

The server refuses to start without a certificate. Create a self-signed pair
once (they are intentionally kept out of version control). The subject
alternative names let the client verify the host by name, including `127.0.0.1`:

```bash
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout tls_key/server.key -out tls_key/server.crt \
  -days 365 -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
```

---

### Build

```bash
cmake -S . -B build
cmake --build build
```

If the dependencies are not on a default search path, point CMake at them:

```bash
cmake -S . -B build \
  -DBOOST_ROOT=C:/path/to/boost_1_88_0 \
  -DOPENSSL_ROOT_DIR=C:/path/to/openssl
```

Both variables are optional; drop whichever one resolves automatically. Setting
`OPENSSL_ROOT_DIR` in the environment works too, and on Windows it must point at
the prefix that contains `include/openssl/ssl.h` and `lib/ssl.lib` (MSVC) or
`lib/libssl.a` (MinGW).

On Windows the compiler also needs `ws2_32` and `mswsock`, which `CMakeLists.txt`
adds automatically. If `CMakeLists.txt` changed, re-run the `cmake -S . -B build`
step so the TLS assets are copied next to the new executable.

---

### Test

The end-to-end suite starts a real server and drives real clients over real TLS
connections. It covers clean transfers, resumption after a kill, rejection of a
tampered file, four clients transferring at once, two clients competing for the
same destination, and the certificate cases that must fail.

```bash
ctest --test-dir build --output-on-failure
```

or run the script directly, optionally keeping the files it creates. On Windows
PowerShell (5.1) it has to be invoked through `powershell`:

```bash
powershell -NoProfile -ExecutionPolicy Bypass -File tests/run_tests.ps1 -BuildDir build
powershell -NoProfile -ExecutionPolicy Bypass -File tests/run_tests.ps1 -BuildDir build -KeepArtifacts
```

It exits non-zero if any check fails, and the suite transfers several hundred
megabytes, so it takes roughly a minute.

---

## Running

Start the server (defaults to port 9000). It must be started from the build
directory, because that is where `tls_key/server.crt` and `tls_key/server.key`
are copied and where uploads are written:

```bash
cd build
./FTP server [port]
```

Send a file to it (defaults to 127.0.0.1:9000):

```bash
./FTP client <path/to/file> [host] [port] [--ca <file> | --insecure]
```

On Windows use `FTP.exe` instead of `./FTP`, for example `.\FTP.exe server`.

Always prefix the executable with `.\` in PowerShell. A bare `FTP.exe` does not
run this program: PowerShell resolves names through `PATH` and finds the
built-in `C:\Windows\System32\ftp.exe` client instead, which prints Microsoft's
FTP help text. `.\FTP.exe` runs the one in the current directory.

A complete session looks like this:

```bash
# terminal 1
cd build
./FTP server 9000
# [SERVER] Listening on port 9000 (TLS enabled)

# terminal 2
echo hello > note.txt
./FTP client ../note.txt 127.0.0.1 9000
# [SUCCESS] Server verified the file (hash matches)
```

The client verifies the server certificate by default. It looks for `server.crt`
next to the executable first and then in `./tls_key`, so it can be launched from
any directory and still trust the self-signed development certificate. The
resolved path is printed before connecting, because a mismatch between the
certificate the client trusts and the one the server holds otherwise shows up
only as `certificate verify failed`.

Uploads land in the server's working directory as `received_<name>`. If the
connection drops, run the same client command again: the server reports how many
bytes it already holds and the transfer continues from there. Once a transfer
completes the server recomputes the SHA-256 of the stored file, deletes it if it
does not match, and tells the client the outcome. The client exits with `0` only
when the server confirmed a matching hash.

Two clients may transfer at the same time, but only one client at a time may
write a given name: a second client asking for a destination that is already in
use is refused as **busy** and exits non-zero, rather than writing over the
transfer in progress.

Progress bars are drawn only when the output is a terminal. With the output
redirected to a file, each session's progress is dropped so the log stays
readable, and only real log lines are written.

---

## Troubleshooting

**The program prints nothing and returns to the prompt instantly.** This is a
DLL failure, not a program bug. Windows rejects the launch with
`0xC0000135` (`STATUS_DLL_NOT_FOUND`) before `main()` runs, so there is no
output and no error message. Check it with:

```powershell
$LASTEXITCODE    # -1073741515 means 0xC0000135, a missing DLL
```

A MinGW build links its runtime and OpenSSL dynamically, so `libstdc++-6.dll`,
`libgcc_s_seh-1.dll`, `libwinpthread-1.dll`, `libssl-*.dll` and `libcrypto-*.dll`
must be reachable. The build copies them next to the executable
(`Runtime DLLs copied next to the executable` appears in the CMake output), so
if they are missing, point CMake at a real toolchain:

```bash
cmake -S . -B build -DOPENSSL_ROOT_DIR=/path/to/openssl
```

On MSVC the runtime is static and this cannot happen.

**`Address already in use`.** A previous server is still listening. End
`FTP.exe` in Task Manager, or pick another port.

**The server logs `handshake error ... http request`.** Something sent plain
HTTP to the TLS port, usually a browser opening `http://localhost:9000`. There
is no web interface; ignore those entries.

---

## Known Limitations

- **Upload only.** There is no download path yet, despite the original design notes.
- **No authentication.** Any client that can reach the port may upload. The bundled
  certificate is self-signed and intended for development, so a real deployment also
  needs a certificate issued for the host it runs on.
- **No per-user storage.** Every upload is stored as `received_<name>` in the server's
  working directory, so a name is shared by everyone. A client that finds the name
  already in use is refused as *busy* rather than being allowed to write over it.
- **Hashing occupies a worker thread.** The digest of a finished transfer is computed
  inline, so a very large file keeps one of the pool's threads busy while it hashes.
- **No transfer encryption beyond TLS.** Payloads are not encrypted a second time and
  there is no signing; integrity relies on the SHA-256 comparison.
- **The protocol is not versioned.** Client and server have to be the same build.

---

## Technologies Used

- C++20
- STL
- Boost.Asio
- OpenSSL
- TLS/SSL
- WinSock
- Threads
- CMake

---

## Design Goals

- Secure communication
- High reliability
- Efficient file transfer
- Modular architecture
- Fault tolerance
- Extensible protocol

---

## Future Improvements

- User authentication
- Directory browsing
- File compression
- Download support
- Per-user storage, so two users can use the same file name
- Upload/download queue
- Recursive folder transfer
- Transfer cancellation
- Persistent transfer metadata
- Logging and monitoring
- IPv6 support
- Cross-platform GUI client

---
