# UML Diagrams

Diagrams for the Secure Resumable File Transfer system, matching
[ARCHITECTURE.md](ARCHITECTURE.md) and the code in `include/` and `src/`.

Contents:

1. [Class diagram](#1-class-diagram)
2. [Sequence diagram — successful transfer](#2-sequence-diagram--successful-transfer)
3. [Sequence diagram — interrupted transfer and resume](#3-sequence-diagram--interrupted-transfer-and-resume)
4. [Sequence diagram — refused request](#4-sequence-diagram--refused-request)
5. [State machine — Session](#5-state-machine--session)
6. [State machine — transfer as a whole](#6-state-machine--transfer-as-a-whole)

---

## 1. Class diagram

```mermaid
classDiagram
    class Server {
        -io_context io_context_
        -ssl::context ctx
        -tcp::acceptor acceptConnect
        -mutex uploads_mutex_
        -unordered_set~string~ active_uploads_
        +Server(io_context, port)
        +stop() void
        +claim_upload(stored_name) bool
        +release_upload(stored_name) void
        +active_uploads() size_t
        -accept_connection() void
    }

    class Session {
        +static atomic~unsigned int~ session_count
        +Session(tcp::socket, ssl::context, Server)
        +start_session() void
        -read_name_length() void
        -on_name_length(ec, transferred) void
        -on_filename(ec, transferred) void
        -on_hash(ec, transferred) void
        -on_file_size(ec, transferred) void
        -write_greeting() void
        -on_greeting_written(ec, transferred) void
        -write_resume_offset() void
        -on_resume_offset_written(ec, transferred) void
        -read_chunk() void
        -on_chunk(ec, transferred) void
        -verify_and_finish() void
        -send_status(status) void
        -on_status_written(ec, transferred) void
        -abort(reason, ec) void
        -release_claim() void
        -shutdown() void
        -report_progress(force) void
        -validate_filename(raw, reason) bool
        -ssl::stream~tcp::socket~ socket_
        -Server server_
        -session_id
        -name_length_
        -incoming_name_
        -expected_hash_
        -file_size_
        -resume_offset_
        -received_
        -stored_file_
        -claimed_
        -ofstream out_
        -vector~char~ chunk_
    }

    class Client {
        -io_context io_context_
        -tcp::resolver resolver_
        -ssl::stream~tcp::socket~ socket_
        -bool succeeded_
        +Client(io_context, ssl_context, host, port, filepath)
        +connect(host, port, filepath) void
        +set_server_name(name) void
        +send_file(path) void
        +handle_error(ec) void
        +close() void
        +fail(reason) void
        +succeeded() bool
    }

    class Checksum {
        +calculate_sha256(filepath, show_progress) string$
    }

    class Status {
        <<enumeration>>
        kProceed = 0
        kOk = 0
        kChecksumMismatch = 1
        kRejected = 2
        kBusy = 3
        kError = 4
    }

    class protocol {
        <<namespace>>
        kChunkSize = 65536
        kHashHexLength = 64
        kMaxNameLength = 255
        kMaxFileSize = 64GiB
    }

    class logging {
        <<namespace>>
        +info(builder) void
        +error(builder) void
        +progress(text) void
        +clear_progress() void
        +stdout_is_terminal() bool
    }

    Server "1" o-- "0..*" Session : creates per client
    Session --> Status : uses
    Session --> protocol : uses
    Client --> Status : reads
    Client --> protocol : uses
    Session ..> Checksum : calls
    Session ..> logging : reports
    Server ..> logging : reports
    Client ..> logging : reports
```

`Server` creates sessions and is the only place that decides whether a
destination is free. A `Session` never talks to another `Session`; concurrent
sessions are kept apart by having one outstanding operation each.

---

## 2. Sequence diagram — successful transfer

```mermaid
sequenceDiagram
    autonumber
    participant U as User
    participant S as Server
    participant Acc as Server::accept_connection
    participant Ss as Session
    participant F as Store

    U->>S: ./FTP server 9000
    S->>Acc: listen(tcp::v4(), 9000)
    Acc-->>S: [SERVER] Listening on port 9000

    U->>S: ./FTP client file.pdf 127.0.0.1 9000
    S->>S: TLS handshake (client, verify CA + host)
    S->>Acc: request arrives

    Acc->>Ss: new Session(socket, ctx, server)
    Acc->>Ss: start_session()
    Ss->>Ss: TLS handshake (server)
    Ss-->>U: [SESSION n] TLS handshake complete

    Ss->>U: async_read uint32 name_len
    U->>Ss: name_len
    Ss->>U: async_read name_len bytes
    U->>Ss: filename
    Ss->>U: async_read 64 bytes
    U->>Ss: expected SHA-256
    Ss->>U: async_read uint64 file_size
    U->>Ss: file_size

    Note over Ss: Request fully received.<br/>Validation happens once, here.

    Ss->>Ss: validate_filename(name)
    Ss->>S: claim_upload("received_" + name)
    alt destination already claimed
        S-->>Ss: false
        Ss->>U: status = kBusy (3)
        Ss->>S: release_claim()
    else claim granted
        S-->>Ss: true
        Ss->>F: stat() for current length
        alt stored length > file_size
            Ss->>F: remove() — stale partial
            Ss->>Ss: resume_offset_ = 0
        else
            Ss->>Ss: resume_offset_ = stored length
        end
        Ss->>U: status = kProceed (0)
        Ss->>U: uint64 resume_offset
        Ss->>F: open(append)

        loop until received_ == file_size_
            Ss->>U: async_read up to 64 KB
            U->>Ss: chunk
            Ss->>F: write(chunk)
            Ss->>Ss: report_progress()
        end

        Ss->>F: flush() + close()
        Ss->>Ss: Checksum::calculate_sha256(stored_file)
        alt digest matches
            Ss->>U: status = kOk (0)
            U-->>U: [SUCCESS] Server verified the file
        else digest differs
            Ss->>F: remove()
            Ss->>U: status = kChecksumMismatch (1)
            U-->>U: [ERROR] Transfer failed
        end
    end

    Ss->>S: release_claim()
    Ss->>U: shutdown TLS
```

## 3. Sequence diagram — interrupted transfer and resume

```mermaid
sequenceDiagram
    autonumber
    participant C as Client
    participant S as Session
    participant F as received_big.bin

    Note over C,S: First attempt — connection dies mid-transfer
    C->>S: header (name, digest, size)
    S-->>C: kProceed + resume_offset = 0
    C->>S: 64 KB chunks...
    C--xS: connection lost
    S->>S: abort("connection lost")
    Note over S: The file is closed, NOT deleted.<br/>The partial bytes stay on disk.
    S-->>F: partial file kept, 9.0 MB of 220 MB

    Note over C,S: Second attempt — same command, new connection
    C->>S: new Session, header again
    S->>F: stat() -> current length
    Note over S: resume_offset is derived from the file<br/>on disk, so there is no checkpoint<br/>state that could disagree with it.
    S-->>C: kProceed + resume_offset = 9.0 MB
    C->>C: lseek(source, 9.0 MB)
    C->>S: only the remaining 211 MB
    S->>F: append
    S->>F: flush, close, hash
    S-->>C: kOk — hashes match
    C-->>C: exit 0
```

The client command is identical both times. Everything needed for resumption is
already on disk, so no session state has to survive the failure.

## 4. Sequence diagram — refused request

```mermaid
sequenceDiagram
    autonumber
    participant C as Client
    participant S as Session
    participant Srv as Server

    C->>S: name = "../../etc/passwd"
    C->>S: digest, file_size
    S->>S: validate_filename("../../etc/passwd")
    Note over S: Rejected: contains a path separator,<br/>and is a directory reference.
    S->>C: status = kRejected (2)
    Note over S: Nothing was written and no<br/>destination was claimed.
    C-->>C: [ERROR] rejected
    C-->>C: exit 1
```

Refusal happens after the header is complete but before any payload, so a
rejected client can never be handed a resume offset it might act on.

---

## 5. State machine — `Session`

```mermaid
stateDiagram-v2
    [*] --> Created

    Created --> Handshaking : start_session
    Handshaking --> ReadingHeader : handshake ok
    Handshaking --> Closed : handshake failed

    state ReadingHeader {
        [*] --> ReadingNameLength
        ReadingNameLength --> ReadingFilename : uint32 name_len
        ReadingFilename --> ReadingHash : name bytes
        ReadingHash --> ReadingFileSize : 64 byte digest
        ReadingFileSize --> [*] : uint64 file_size
    }

    ReadingHeader --> Validating : header complete
    ReadingHeader --> Closed : connection lost

    Validating --> Refused : invalid name / limit exceeded
    Validating --> CheckingClaim : name accepted

    CheckingClaim --> Refused : kBusy (already claimed)
    CheckingClaim --> DecidingOffset : claim granted

    DecidingOffset --> DiscardingStale : stored length > file_size
    DecidingOffset --> SendingProceed : stored length <= file_size
    DiscardingStale --> SendingProceed : partial file removed

    SendingProceed --> Receiving : kProceed + resume_offset
    Refused --> Closed : status sent

    state Receiving {
        [*] --> ReadingChunk
        ReadingChunk --> WritingChunk : up to 64 KB
        WritingChunk --> ReadingChunk : received_ < file_size_
        WritingChunk --> [*] : received_ == file_size_
    }

    Receiving --> Verifying : all bytes stored
    Receiving --> Closed : connection lost (partial kept)

    Verifying --> Verified : digest matches
    Verifying --> DeletingCorrupt : digest differs
    DeletingCorrupt --> Failed : file removed

    Verified --> SendingStatus
    Failed --> SendingStatus
    SendingStatus --> Closed : kOk or kChecksumMismatch

    Closed --> [*]
```

Two transitions matter most. `Receiving --> Closed` deliberately **keeps** the
partial file, because that file is the resumption state. `Verifying -->
DeletingCorrupt` removes a file that failed its hash, so a corrupt upload is
never left looking like a successful one.

## 6. State machine — transfer as a whole

```mermaid
stateDiagram-v2
    [*] --> Idle

    Idle --> Connecting : client starts
    Connecting --> Handshaking : TCP established
    Connecting --> Failed : host unreachable / refused

    Handshaking --> Verifying : TLS negotiation
    Handshaking --> Failed : certificate rejected

    Verifying --> RequestSent : chain + host name valid
    Verifying --> Failed : verification failed

    RequestSent --> RefusedBusy : status = kBusy
    RequestSent --> RefusedInvalid : status = kRejected
    RequestSent --> Resuming : status = kProceed

    RefusedBusy --> Failed
    RefusedInvalid --> Failed

    Resuming --> Transferring : offset > 0 (resuming)
    Resuming --> Transferring : offset = 0 (fresh)

    Transferring --> Interrupted : connection lost
    Interrupted --> [*] : partial file kept

    Transferring --> VerifyingHash : all bytes sent

    VerifyingHash --> Succeeded : server confirms kOk
    VerifyingHash --> CorruptRejected : server reports mismatch

    Succeeded --> [*] : exit 0
    CorruptRejected --> [*] : file deleted, exit 1
    Failed --> [*] : exit 1
```

`Interrupted` is a dead end for that run, but not for the transfer: rerunning
the identical command re-enters at `Resuming`, and the offset is recomputed from
the file on disk.
