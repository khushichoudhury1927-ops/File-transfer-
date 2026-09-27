# Development Plan and Progress

**Project:** Secure Resumable File Transfer (SRFT)

This document maps the six required stages onto the work actually carried out,
records the evidence for each, and sets out the plan for what remains.

---

## 1. Stage status summary

| Stage | Deliverable | Document | Status |
| --- | --- | --- | --- |
| 1 | Project introduction, problem, scope, outcome | `README.md` §Overview, §Features | Complete |
| 2 | Requirements and development plan | `docs/PRD.md`, this document | Complete |
| 3 | System design, architecture, UML, environment, Git | `docs/ARCHITECTURE.md`, `docs/UML.md` | Complete, with one deviation (§4) |
| 4 | Initial implementation and prototype | `src/`, `include/` | Complete |
| 5 | Testing, integration, improvement | `docs/TEST_PLAN.md`, `tests/run_tests.ps1` | Complete |
| 6 | Final implementation and presentation | `README.md`, `docs/` | Source complete; report and presentation outstanding |

## 2. Stage detail

### Stage 1 — Project introduction

| Item | Outcome |
| --- | --- |
| Problem | Transfers either restart from zero or complete without proof of integrity, and data travels in clear text. |
| Objective | A TLS-encrypted, resumable transfer that proves the delivered file matches the source. |
| Scope | Upload-only service, no authentication, no download. See `docs/PRD.md` §1.3. |
| Expected outcome | A command line tool that recovers from interruptions and verifies every transfer. |
| Application | A machine that must reliably *receive* files over an unstable link. |

**Evidence:** `README.md` overview and features sections; `docs/PRD.md` §1.

### Stage 2 — Requirements and development plan

| Item | Outcome |
| --- | --- |
| Functional requirements | 20 requirements, FR-01 to FR-20, each traced to code and to at least one automated check. |
| Non-functional requirements | 12 requirements, NFR-01 to NFR-12. |
| Constraints | 4 system constraints recorded. |
| Known limitations | 7 recorded, most importantly the absence of a download path. |
| Deliverables | 8 artefacts listed. |
| Success criteria | 7 measurable criteria. |

**Evidence:** `docs/PRD.md`; this document.

### Stage 3 — System design and architecture

| Item | Outcome |
| --- | --- |
| Architecture | Five layers with a one-directional dependency rule; see `docs/ARCHITECTURE.md` §2. |
| Component responsibilities | `Server`, `Session`, `Client`, `Checksum`, `protocol`, `logging`, each documented. |
| Data structures | 7 structures with the reason each type was chosen. |
| Class diagram | `docs/UML.md` §1. |
| Sequence diagrams | Successful transfer, interruption and resume, refusal. |
| State machine diagrams | `Session` lifecycle and end-to-end transfer lifecycle. |
| Implementation plan | Handler chain per stage, §3 of the architecture document. |
| Development environment | CMake with OpenSSL and header-only Boost; MinGW and MSVC both supported. |
| Git repository | Configured with a `main` branch and descriptive commits. |
| Documentation | `README.md` plus this `docs/` set. |

**Evidence:** `docs/ARCHITECTURE.md`, `docs/UML.md`.

### Stage 4 — Initial implementation and prototype

Implementation order, which reflects the dependency structure:

| Order | Component | Notes |
| --- | --- | --- |
| 1 | `protocol.hpp` | Wire constants and status codes first, so both sides share a fixed contract. |
| 2 | `logging.hpp` | Needed early: every later component reports progress and errors through it. |
| 3 | `checksum.cpp` | Standalone, no networking, so it could be validated on its own. |
| 4 | `server.cpp` | Acceptor, shared SSL context, destination claim set. |
| 5 | `serverSession.cpp` | The core: request validation, chunk loop, integrity verdict. |
| 6 | `client.cpp` | Resolve, connect, send header, honour the resume offset. |
| 7 | `main.cpp` | CLI parsing, certificate discovery, host verification callback, event loop. |

**Evidence:** the module set in `src/` and `include/`, and the demonstration
recorded in §5 below.

### Stage 5 — Testing, integration and improvement

| Item | Outcome |
| --- | --- |
| Test strategy | End-to-end only: a real server, real clients, real TLS, no mocks. |
| Coverage | 50 automated checks across 8 groups. |
| Toolchains | Verified on GCC 16 (Ninja) and GCC 15.2 (CLion, MinGW). |
| Defects found and fixed | Listed in `docs/TEST_PLAN.md` §5. |
| Performance | 8 MB transferred at ~267 MB/s on loopback; 220 MB resumed transfer verified. |
| Code quality | Single translation unit per component, no framework, comments explain intent rather than syntax. |

**Evidence:** `docs/TEST_PLAN.md`, `tests/run_tests.ps1`.

### Stage 6 — Final implementation and presentation

| Item | Status |
| --- | --- |
| Working system | Complete and demonstrated. |
| Architecture, implementation, testing write-up | `docs/ARCHITECTURE.md`, `docs/UML.md`, `docs/TEST_PLAN.md`. |
| Source code and Git repository | Complete. |
| Achievements, limitations, future work | `README.md` §Known Limitations, `docs/PRD.md` §6. |
| **Final project report** | **Outstanding** — a single consolidated document is still to be written. |
| **Presentation** | **Outstanding** — a demonstration script should be prepared. |

## 3. Development timeline

The schedule below is the plan for the remaining work, expressed in working
days from the start of Stage 6. Stages 1 to 5 are complete in the repository.

| Day | Task | Output |
| --- | --- | --- |
| 1 | Write the consolidated project report: introduction, requirements, design, implementation, testing, results, conclusion. | `docs/PROJECT_REPORT.md` |
| 2 | Add a measured results table to the report from a fresh full-suite run. | Report §Results |
| 3 | Rehearse the demonstration: clean build, certificate generation, single transfer, interrupted transfer, four concurrent uploads. | Rehearsal notes |
| 4 | Record the demonstration and check the timing of the bulk phase. | Presentation evidence |
| 5 | Proofread the report against `docs/`, and re-verify every claim against the code. | Final report |
| 6 | Submit source, documentation, diagrams, repository and report. | Submission |

## 4. Deviations and risks

### 4.1 The device driver component is not covered

The brief for this project also requires **Linux Device Drivers**. The
implemented system is C++20 and system-level networking, but contains no kernel
module, so that part of the brief is **not** satisfied by this repository.

Options, in order of effort:

1. **Add a character device driver** to this project: a kernel module exposing
   `/dev/ftprecv` that the server uses as its storage backend, handling
   `open`, `write`, `lseek` and `ioctl`, plus a `sysfs` interface for
   statistics. This reuses all existing work and covers all three subjects in
   one project. It requires a Linux environment; WSL2 with Ubuntu is already
   installed and is sufficient for a character device driver.
2. **Submit a separate driver project** for the device driver component.
3. **Accept the gap** if the course weights the driver requirement across
   multiple projects.

This is a decision for the project owner and should be settled before the final
report is written, because it changes the scope statement.

### 4.2 Git history does not yet show stage-by-stage progress

The brief asks for visible continuous progress with commits at every stage. The
repository currently has three commits, all created after the implementation was
substantially complete. That is the weakest part of the submission as evidence
of process.

Recommended remedy, in order of effort:

1. From now on, make one commit per stage or per completed feature, with
   messages that explain *why* the change was made.
2. If a per-stage history is required retroactively, reconstruct it honestly by
   branching from the current state and documenting which work belonged to
   which stage in the report, rather than rewriting history to imply a
   schedule that did not happen.

### 4.3 No download path

The system receives files but cannot return them, so it cannot yet be used to
share files with a third party. This is recorded as limitation 1 in
`docs/PRD.md` §6 and is the highest-value feature to add.

## 5. Demonstration script

Use this order; it takes about five minutes and shows each capability in
ascending order of sophistication.

| Step | Action | What it demonstrates |
| --- | --- | --- |
| 1 | `cmake -S . -B build && cmake --build build` | Reproducible build from source. |
| 2 | `ctest --test-dir build --output-on-failure` | 50 automated checks passing. |
| 3 | Start the server, send one small file. | Basic TLS transfer with verification. |
| 4 | Show the stored file and compare hashes. | Integrity guarantee, proved independently. |
| 5 | Start a large transfer, kill the client mid-transfer, show the partial file, rerun the same command. | Resumption without retransmitting the whole file. |
| 6 | Start four clients at once. | Concurrency, and one writer per destination. |
| 7 | Point a client at the LAN address instead of `127.0.0.1`. | Certificate verification refusing an address the certificate does not cover. |
| 8 | Show the refusal log entry for a traversal-style file name. | Input validation and path traversal defence. |

Steps 5 to 8 are the ones that distinguish this from a file copy, because each
demonstrates a property that a plain copy does not have.
