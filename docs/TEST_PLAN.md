# Test Plan and Results

**Project:** Secure Resumable File Transfer (SRFT)

---

## 1. Strategy

The suite is **end-to-end only**. Every check starts a real server process and
drives real client processes against it over real TLS connections. Nothing is
mocked or stubbed.

The reasoning is that this system's value lies entirely in behaviour that spans
processes: a socket, a handshake, a file on disk, a second process writing to
the same destination. A unit test with a fake socket would pass while the real
program failed, so it would test the wrong thing. The cost is that the suite
transfers several hundred megabytes and takes roughly a minute.

| Level | Covered by |
| --- | --- |
| Unit | Deliberately minimal. Only `parse_port` and `validate_filename` have pure logic, and both are exercised through the command line and through hostile file names. |
| Integration | Every check is an integration check: a real client against a real server. |
| System | The full suite, plus the manual demonstration in `docs/DEVELOPMENT_PLAN.md` §5. |

## 2. Entry and exit criteria

| Criterion | Result |
| --- | --- |
| Every functional requirement FR-01 to FR-20 is exercised | Yes, see the traceability matrix in §3 |
| Suite passes on two independent toolchains | Yes, GCC 16 and GCC 15.2 |
| Suite passes with an empty `PATH` | Yes, no OpenSSL or MinGW directory required |
| Suite is repeatable | Yes, run repeatedly during development with no flakes |
| Suite exits non-zero on failure | Yes, so CTest and CI can gate on it |

## 3. Coverage and traceability

| Group | Checks | Requirements covered | What it proves |
| --- | --- | --- | --- |
| Command line handling | 10 | FR-17, FR-19 | No arguments prints usage and exits 1. Unknown mode exits 1. A client with no file exits 1. Ports `0`, `70000`, `abc`, `65536` and `-1` are each rejected with a message rather than silently wrapping. An unknown option is refused. |
| Clean transfer | 6 | FR-01, FR-02, FR-06, FR-14, FR-19 | The client exits 0, reports verification, stores the file as `received_<name>`, and the stored size, SHA-256 and byte content all match the source. |
| Certificate verification | 4 + 1 skip | FR-03, FR-04, FR-05 | The correct CA is accepted when connecting by IP literal and by DNS name. `--insecure` connects with verification off. A missing CA file is reported rather than silently ignored. An unrelated CA is rejected (skipped where the `openssl` tool is unavailable to generate a second certificate). |
| Integrity | 5 | FR-12, FR-13, FR-18 | A payload altered in flight is refused, the client is told the transfer failed, the server deletes the corrupt file, an empty file is refused by the client, and a missing source file is reported. |
| Resuming | 5 | FR-08, FR-09 | A 24 MB file uploads, a resumed transfer reports the offset it started from, only the missing bytes are sent, the resumed file matches, and a stored partial **larger** than the source is discarded and re-uploaded. |
| Interruption | 5 | FR-20 | The client is killed mid-transfer, a partial file is kept, the next run resumes, the reported offset matches the partial size, and the completed 220 MB file matches the source. |
| Concurrency | 14 | FR-10, FR-11 | A second client is served while a long transfer runs, and finishes quickly. Four parallel uploads all succeed and are intact. A competing client for the same destination is refused, the refusal says *busy*, and the original transfer still completes cleanly. |
| Server without a certificate | 1 | FR-16 | The server refuses to start when no certificate is available. |
| **Total** | **50 passed, 1 skipped** | FR-01 to FR-20 | |

## 4. Environment

| Component | Version |
| --- | --- |
| OS | Windows 11 Home, x64 |
| Toolchain A | GCC 16.x, MSYS2 UCRT64, Ninja generator |
| Toolchain B | GCC 15.2, MinGW-w64 bundled with CLion, MinGW Makefiles generator |
| TLS | OpenSSL 3.6.4 |
| Boost | 1.88, headers only |
| CMake | 3.15 or newer |
| Test driver | Windows PowerShell 5.1 |

The suite is run through CTest:

```
ctest --test-dir build --output-on-failure
```

or directly, optionally keeping the files it generates:

```
powershell -NoProfile -ExecutionPolicy Bypass -File tests/run_tests.ps1 -BuildDir build
powershell -NoProfile -ExecutionPolicy Bypass -File tests/run_tests.ps1 -BuildDir build -KeepArtifacts
```

## 5. Defects found and fixed

Every item below was found by the suite or by manual testing, and each has a
regression check or a documented behaviour.

| # | Symptom | Root cause | Fix |
| --- | --- | --- | --- |
| 1 | Server could not replace a stale upload on Windows | A file left open elsewhere could not be deleted, and the failure was swallowed | Report the failure and abort the transfer explicitly |
| 2 | A killed client's failure was not visible to the script | The exit code was read from a process object that had not been waited on | Wait for the process, then read `ExitCode` |
| 3 | The *busy* check failed intermittently | The test assumed a transfer was in progress when it had already finished | Wait for evidence that bytes are actually moving before starting the competing client |
| 4 | Concurrent sessions produced garbled log lines | Two threads wrote to the same stream mid-line | Serialise every write under one mutex |
| 5 | A redirected server log was buried under progress updates | The progress bar was written even when output was not a terminal | Emit progress only when stdout is a terminal |
| 6 | Transfer rate printed as `0.0286102` | The value was written to a shared stream, inheriting default float precision | Set `fixed` / `setprecision(2)` explicitly at the write site |
| 7 | The MinGW build produced no output at all and returned instantly | The runtime and OpenSSL DLLs were not shipped, so the loader aborted with `0xC0000135` before `main` ran | CMake copies the DLLs next to the executable and warns if the OpenSSL ones are missing |
| 8 | The client failed to verify the certificate when launched from another directory | The CA was looked up in the working directory first, so an unrelated `tls_key` won | Look next to the executable first, and log the certificate actually trusted |

Item 7 is worth singling out. It presented as a silent failure with no error
message, which is the worst possible shape for a defect, and it only appeared on
the MinGW toolchain. It is now covered by a check that the runtime is staged,
and documented in the troubleshooting section of the `README`.

## 6. Results

### 6.1 Automated suite

| Toolchain | Result |
| --- | --- |
| GCC 16, Ninja | 50 passed, 0 failed, 1 skipped |
| GCC 15.2, MinGW Makefiles | 50 passed, 0 failed, 1 skipped |
| Either, with an empty `PATH` | 50 passed, 0 failed, 1 skipped |

The single skip is the "an unrelated CA is rejected" check, which needs the
`openssl` command line tool to generate a second certificate. It is reported as
a skip rather than a pass so that the count never overstates coverage.

### 6.2 Measured behaviour

Measured on loopback, so these figures indicate overhead and correctness rather
than network throughput.

| Scenario | Result |
| --- | --- |
| Single 8 MB transfer | Completed in 0.03 s, ~267 MB/s, digests matched |
| 220 MB transfer, interrupted then resumed | Resumed from the partial offset, final digest matched |
| 24 MB transfer, only the missing bytes sent | Verified: the client transmitted the remainder, not the whole file |
| Four concurrent uploads | All four completed and were byte-for-byte intact |
| Real PDF invoices from a 997-file collection | Individual files transferred and verified. **The whole collection has not been sent**; see §6.4. |

### 6.3 Manual verification

Beyond the suite, a real 14,466-byte PDF invoice was transferred and then
checked independently of the program's own reporting: the SHA-256 of the stored
file was recomputed with an external tool and compared with the source, the file
sizes were compared, a byte-for-byte comparison was performed, and the PDF
structure was confirmed (`%PDF-1.4` header, `%%EOF` trailer, `xref`, `startxref`,
one page object, not encrypted). This deliberately does not rely on the
program's own `[SUCCESS]` line as evidence.

## 6.4 Not yet done

The following were identified as useful but have **not** been performed, and no
result is claimed for them:

- Transferring the full collection of 997 invoices. Individual files from the
  collection were transferred and verified; a bulk run has not been carried out.
- Any cross-machine transfer.
- Any Linux or macOS build or test run.

## 7. Coverage gaps

| Not covered | Reason |
| --- | --- |
| Linux and macOS builds | Developed and verified on Windows only. The code has POSIX paths for Linux, but they are untested. |
| Real network conditions | All tests run over loopback. Packet loss, latency, MTU limits and reordering are simulated only by killing the client, which does not reproduce them faithfully. |
| Two separate machines | The certificate covers `localhost` and `127.0.0.1` only, so a LAN address is correctly refused. A cross-machine test needs a certificate issued for that address. |
| Download path | Not implemented; see `docs/PRD.md` §6. |
| Authentication | Not implemented, so there is nothing to test. |
| Very large files near the 64 GiB limit | The limit is enforced in code but not exercised; testing it would need more disk than is available. |
| Long-running soak | Sessions are short-lived in the suite. A multi-day run would be needed to expose leaks or descriptor exhaustion. |

## 8. Future test work

In priority order:

1. Run the suite on Linux, ideally under WSL2, to cover the untested platform.
2. Add a test that transfers between two machines on a LAN, with a certificate
   issued for the server's address, to prove the cross-machine path.
3. Introduce latency and packet loss with a proxy between client and server, to
   test resumption under realistic conditions rather than by killing a process.
4. Add a test for the 64 GiB limit that uses a sparse file, so it does not need
   real disk space.
5. Add a leak check over many sequential sessions, watching file descriptor
   count.
