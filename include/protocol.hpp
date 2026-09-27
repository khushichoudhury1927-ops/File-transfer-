#ifndef PROTOCOL_HPP
#define PROTOCOL_HPP

#include <cstddef>
#include <cstdint>

// Wire format shared by the client and the server.
//
// All scalars are sent in host byte order and are read/written with a single
// boost::asio::write/read call, so no packing is required.
//
//   client -> server : uint32 name_len | name_len bytes filename
//                      | 64 bytes hex hash | uint64 file_size
//   server -> client : uint8 status   (kProceed, or the reason it was refused)
//                      | uint64 resume_offset        (only when kProceed)
//   client -> server : file_size - resume_offset bytes of payload
//   server -> client : uint8 status   (the final verdict)
//
// The whole request is validated before anything is written back, so a refused
// request is reported as a status byte instead of a bogus resume offset.
namespace protocol {

// Payload size of a single transfer chunk.
constexpr std::size_t kChunkSize = 65536;  // 64 KB

// Length of a SHA-256 digest rendered as lowercase hex.
constexpr std::size_t kHashHexLength = 64;

// Sanity limits applied to values received over the wire.
constexpr std::size_t kMaxNameLength = 255;
constexpr std::uint64_t kMaxFileSize = 64ull * 1024 * 1024 * 1024;  // 64 GiB

// Verdict the server sends back. The same set is used twice: once before the
// payload to accept or refuse the request, and once at the end to report
// whether the file arrived intact.
enum class Status : unsigned char {
    kProceed = 0,  // request accepted, the payload may be sent
    kOk = 0,       // final verdict: the file was verified
    kChecksumMismatch = 1,
    kRejected = 2,  // request refused before any payload was read
    kBusy = 3,     // another session is already uploading this name
    kError = 4,
};

inline const char* to_string(Status status) {
    switch (status) {
        // kOk and kProceed share the value 0, so one label covers both.
        case Status::kProceed:
            return "ok";
        case Status::kChecksumMismatch:
            return "checksum mismatch";
        case Status::kRejected:
            return "rejected";
        case Status::kBusy:
            return "busy";
        case Status::kError:
            break;
    }
    return "error";
}

}  // namespace protocol

#endif // PROTOCOL_HPP
