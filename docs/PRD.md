# Project Requirements Document (PRD)

**Project:** Secure Resumable File Transfer (SRFT)
**Version:** 1.0
**Language / Platform:** C++20, cross-platform (Windows, Linux)
**Repository:** <https://github.com/zexxitywave/File-transfer->

---

## 1. Introduction

### 1.1 Problem statement

Bulk file transfer over an unreliable network is usually handled by tools that
either restart a transfer from zero or offer no proof that the delivered file is
the file that was sent. Three problems follow from this:

1. **Wasted bandwidth.** A connection that drops at 99% forces a full re-send
   under naive protocols.
2. **Unverifiable delivery.** A transfer can complete "successfully" while the
   received file is corrupt or truncated, and neither party notices.
3. **Unprotected transit.** File contents travel in clear text, so anyone on the
   network path can read or alter them.

Existing tools address these individually rather than together, and general
purpose file transfer daemons often add authentication and directory browsing
features that are unnecessary for a machine-to-machine transfer service.

### 1.2 Objective

Build a command line file transfer service in C++20 that sends files over a
TLS-encrypted connection, resumes automatically after a network interruption,
and proves the delivered file is byte-for-byte identical to the source.

### 1.3 Scope

**In scope**

- A single binary that runs in one of two modes: `server` (receives) or
  `client` (sends).
- TLS-encrypted transport with certificate and host name verification.
- Resumption of an interrupted transfer from the last stored byte.
- SHA-256 integrity verification of every completed transfer.
- Concurrent transfers, with mutual exclusion per destination file name.
- Strict validation of client-supplied file names.
- An automated end-to-end test suite.

**Out of scope**

- File download / retrieval from the server.
- User authentication and authorisation.
- Per-user storage namespaces.
- Directory browsing.
- File compression.
- Payload encryption beyond TLS, and digital signing.
- A graphical interface.

### 1.4 Intended application

A machine that must quietly and reliably *receive* files, such as a backup
target, an upload endpoint on an embedded device, or a lab machine collecting
measurement data from several clients over an unstable link.

---

## 2. Stakeholders

| Role | Interest |
| --- | --- |
| Machine receiving files | Availability, integrity, no partial files left behind |
| Machine sending files | Resumption, verifiable completion, clear errors |
| Network operator | Encrypted transit, no clear-text credentials or data |
| Maintainer | Testable code, documented protocol, portable build |

---

## 3. Functional requirements

Each requirement is traceable to the implementation and to at least one
automated check.

| ID | Requirement | Implementation | Verified by |
| --- | --- | --- | --- |
| FR-01 | The system shall operate as a server that accepts TLS connections on a configurable TCP port, default 9000. | `Server`, `main.cpp:run_server` | Clean transfer checks |
| FR-02 | The system shall operate as a client that uploads a named file to a configurable host and port, default `127.0.0.1:9000`. | `Client`, `main.cpp:run_client` | Clean transfer checks |
| FR-03 | All payload and control data shall travel inside a TLS session. | `ssl::stream<tcp::socket>` in both roles | Certificate verification checks |
| FR-04 | The client shall verify the server certificate chain and confirm the certificate names the host it connected to, by default. | `make_host_check`, `set_verify_mode` | Certificate verification checks |
| FR-05 | The client shall support `--ca <file>` to verify against a chosen authority and `--insecure` to disable verification with a warning. | `main.cpp:263-324` | Certificate verification checks |
| FR-06 | The client shall send the destination name, the expected SHA-256 digest and the file size as a header before any payload. | `protocol.hpp`, `Client::send_file` | Clean transfer checks |
| FR-07 | The server shall validate the request completely before storing any byte of payload. | `Session::on_file_size` | Command line handling, integrity checks |
| FR-08 | The server shall report the number of bytes it already holds, and the client shall resume from exactly that offset. | `write_resume_offset`, `Session::on_file_size` | Resuming checks |
| FR-09 | A stored file larger than the announced source size shall be treated as stale, discarded, and re-uploaded from offset 0. | `Session::on_file_size` | Resuming: stale oversized partial |
| FR-10 | The server shall accept several clients concurrently. | `run_event_loop`, thread pool | Concurrency: four parallel uploads |
| FR-11 | Only one client at a time shall be permitted to write a given destination name; a second shall be refused as busy. | `Server::claim_upload` | Concurrency: competing client refused |
| FR-12 | The server shall compute the SHA-256 of the stored file and compare it to the digest supplied by the client. | `Checksum::calculate_sha256`, `verify_and_finish` | Integrity: tampered file refused |
| FR-13 | A file whose digest does not match shall be deleted and the client informed; the client shall exit non-zero. | `verify_and_finish`, `send_status` | Integrity checks |
| FR-14 | Uploads shall be stored as `received_<name>` in the server's working directory. | `Session::on_file_size` | Clean transfer: stored under `received_` |
| FR-15 | Client-supplied file names shall be rejected if empty, a directory reference, longer than 255 bytes, containing control characters or any of `/\:*?"<>|`, ending in a space or dot, or a reserved device name. | `Session::validate_filename` | Command line handling, integrity checks |
| FR-16 | The server shall refuse to start when no certificate is available. | `run_server`, `Server` construction | Server without a certificate |
| FR-17 | The client shall reject an invalid port (`0`, above 65535, or non-numeric) with a clear message. | `parse_port` | Command line handling checks |
| FR-18 | The client shall report a missing or unreadable source file before transferring. | `Client::send_file` | Integrity: missing source reported |
| FR-19 | The client shall exit 0 only when the server confirmed a matching digest. | `Client::succeeded`, `run_client` | Clean transfer: client exits 0 |
| FR-20 | An interrupted transfer shall leave a partial file that a later run resumes from. | `abort`, resume logic | Interruption checks |

## 4. Non-functional requirements

| ID | Requirement | Rationale | Status |
| --- | --- | --- | --- |
| NFR-01 | A transfer shall place at most one asynchronous operation in flight, so a session never occupies a worker thread. | Prevents head-of-line blocking between clients. | Implemented: asynchronous handler chain in `Session` |
| NFR-02 | The server shall run its event loop on a small thread pool, sized `min(4, hardware_concurrency)` with a floor of 2. | Bounds resource use while allowing concurrency. | Implemented: `server_thread_count` |
| NFR-03 | Hashing a very large file shall not block the accept loop. | Keeps the server responsive. | Partial: hashing occupies one pool thread; noted in §6 |
| NFR-04 | Memory use shall be bounded regardless of file size; chunks are streamed, never buffered whole. | Predictable footprint on small devices. | Implemented: fixed 64 KB `chunk_` buffer |
| NFR-05 | All scalars shall be sent in host byte order as single writes, so no wire struct packing is required. | Removes a classic source of corruption bugs. | Implemented: `protocol.hpp` |
| NFR-06 | The build shall be portable across Windows and Linux with one CMake script. | Course and deployment flexibility. | Implemented: `CMakeLists.txt` |
| NFR-07 | Concurrent sessions shall not interleave partial log lines. | Readability of server logs. | Implemented: mutex-protected logging |
| NFR-08 | Progress output shall appear only on a terminal; redirected output shall contain only real log lines. | Keeps log files usable. | Implemented: `logging::stdout_is_terminal` |
| NFR-09 | A MinGW build shall run without the user modifying `PATH`. | The executable otherwise fails to start with `0xC0000135`. | Implemented: runtime DLL copy in CMake |
| NFR-10 | The test suite shall start a real server and drive real clients over real TLS, with no mocking. | Tests the shipped path, not a stub. | Implemented: `tests/run_tests.ps1` |
| NFR-11 | The build shall not require linking Boost libraries, since Asio is header-only. | Simplifies dependency setup. | Implemented: header-only usage |
| NFR-12 | No certificate or private key shall be committed to version control. | Avoids shipping key material. | Implemented: `.gitignore`, generated at setup |

## 5. System constraints

| Constraint | Effect |
| --- | --- |
| Client and server must be the same build | The wire protocol carries no version field; see §6 |
| A transfer can only resume against the same stored file | Resumption depends on the stored length of `received_<name>` |
| TLS certificate must cover the address used | Verification fails by design for an address outside the certificate's SAN |
| Uploads land in the server's working directory | Storage location follows the directory the server was started from |

## 6. Known limitations

1. **Upload only.** There is no download path, so the service cannot yet be
   used to *share* files. This is the most significant gap.
2. **No authentication.** Any client that can reach the port may upload.
3. **Shared namespace.** Every upload becomes `received_<name>`, so all clients
   share one flat name space. A name already in use is refused as busy.
4. **Hashing occupies a worker thread.** The digest of a finished transfer is
   computed inline, so a very large file keeps one pool thread busy.
5. **The protocol is not versioned.** Client and server must be the same build;
   a version field is required before independent releases can interoperate.
6. **No cancellation.** Once a transfer starts it runs to completion or fails.
7. **Bundled certificate is self-signed** and intended for development only.

## 7. Deliverables

| Deliverable | Location |
| --- | --- |
| Source code | `src/`, `include/` |
| Build system | `CMakeLists.txt` |
| Automated test suite | `tests/run_tests.ps1` |
| Requirements document | `docs/PRD.md` (this file) |
| Architecture and component design | `docs/ARCHITECTURE.md` |
| UML diagrams | `docs/UML.md` |
| Development plan and progress | `docs/DEVELOPMENT_PLAN.md` |
| Test plan and results | `docs/TEST_PLAN.md` |
| User and build documentation | `README.md` |

## 8. Success criteria

The project is considered complete when:

1. A file transferred on a clean run is byte-for-byte identical to the source
   and the server reports a matching digest.
2. A transfer killed mid-flight resumes from the correct offset on the next run
   and produces an identical file.
3. A corrupted payload is detected, the stored file deleted, and the client
   exits non-zero.
4. Four concurrent uploads all succeed and remain intact.
5. A competing client for the same destination is refused as busy.
6. The full suite passes on two independent toolchains.
7. The binary starts and runs on Windows and Linux from the same source with no
   manual `PATH` edits.
