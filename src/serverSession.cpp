#include "serverSession.hpp"

#include "checksum.hpp"
#include "logging.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <iomanip>
#include <sstream>
#include <string>

namespace {

// Payload of every transfer lives next to the server as "received_<name>".
constexpr const char* kStoredFilePrefix = "received_";

constexpr int PROGRESS_INTERVAL_MS = 500;

bool is_sha256_hex(const std::string& value) {
    if (value.size() != protocol::kHashHexLength) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

// Windows refuses these as file names, and they would silently disappear
// instead of being stored, so they are turned away up front.
bool is_reserved_device_name(const std::string& name) {
    std::string base = name.substr(0, name.find('.'));
    for (char& c : base) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    static const char* kReserved[] = {"con", "prn",  "aux", "nul", "com1", "com2", "com3",
                                      "com4", "com5", "com6", "com7", "com8", "com9",
                                      "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6",
                                      "lpt7", "lpt8", "lpt9"};
    return std::any_of(std::begin(kReserved), std::end(kReserved),
                       [&base](const char* r) { return base == r; });
}

}  // namespace

std::atomic<unsigned int> Session::session_count{0};

Session::Session(tcp::socket socket, ssl::context& ctx, Server& server)
    : socket_(std::move(socket), ctx),
      server_(server),
      session_id(session_count.fetch_add(1) + 1),
      chunk_(protocol::kChunkSize) {
    logging::info([&](std::ostream& out) { out << "SESSION[" << session_id << "] created"; });
}

bool Session::validate_filename(const std::string& raw, std::string& reason) {
    if (raw.empty()) {
        reason = "empty file name";
        return false;
    }
    if (raw == "." || raw == "..") {
        reason = "file name is a directory reference";
        return false;
    }
    if (raw.size() > protocol::kMaxNameLength) {
        reason = "file name is longer than " + std::to_string(protocol::kMaxNameLength) + " bytes";
        return false;
    }
    for (const unsigned char c : raw) {
        if (c < 0x20 || c == 0x7f) {
            reason = "file name contains a control character";
            return false;
        }
        if (std::strchr("/\\:*?\"<>|", static_cast<char>(c)) != nullptr) {
            reason = "file name contains a path or reserved character";
            return false;
        }
    }
    if (raw.back() == ' ' || raw.back() == '.') {
        reason = "file name ends with a space or a dot";
        return false;
    }
    if (is_reserved_device_name(raw)) {
        reason = "file name is a reserved device name";
        return false;
    }
    return true;
}

void Session::start_session() {
    auto self = shared_from_this();  // keep the session alive across async ops

    socket_.async_handshake(
        ssl::stream_base::server, [self](const bsys::error_code& ec) {
            if (ec) {
                self->abort("TLS handshake failed", ec);
                return;
            }
            logging::info([&](std::ostream& out) {
                out << "[SESSION " << self->session_id << "] TLS handshake complete";
            });
            self->read_name_length();
        });
}

void Session::read_name_length() {
    auto self = shared_from_this();
    boost::asio::async_read(socket_, boost::asio::buffer(&name_length_, sizeof(name_length_)),
                            [self](const bsys::error_code& ec, std::size_t transferred) {
                                self->on_name_length(ec, transferred);
                            });
}

void Session::on_name_length(const bsys::error_code& ec, std::size_t) {
    if (ec) {
        abort("could not read the file name length", ec);
        return;
    }
    if (name_length_ == 0 || name_length_ > protocol::kMaxNameLength) {
        const std::string detail = "invalid file name length: " + std::to_string(name_length_);
        logging::error([&](std::ostream& out) {
            out << "[SESSION " << session_id << "] Rejected: " << detail;
        });
        send_status(protocol::Status::kRejected);
        return;
    }

    incoming_name_.assign(name_length_, '\0');
    auto self = shared_from_this();
    boost::asio::async_read(socket_, boost::asio::buffer(incoming_name_),
                            [self](const bsys::error_code& e, std::size_t transferred) {
                                self->on_filename(e, transferred);
                            });
}

void Session::on_filename(const bsys::error_code& ec, std::size_t) {
    if (ec) {
        abort("could not read the file name", ec);
        return;
    }

    auto self = shared_from_this();
    expected_hash_.resize(protocol::kHashHexLength);
    boost::asio::async_read(socket_, boost::asio::buffer(expected_hash_),
                            [self](const bsys::error_code& e, std::size_t transferred) {
                                self->on_hash(e, transferred);
                            });
}

void Session::on_hash(const bsys::error_code& ec, std::size_t) {
    if (ec) {
        abort("could not read the expected hash", ec);
        return;
    }
    if (!is_sha256_hex(expected_hash_)) {
        logging::error([&](std::ostream& out) {
            out << "[SESSION " << session_id << "] Rejected: malformed SHA-256 from the client";
        });
        send_status(protocol::Status::kRejected);
        return;
    }

    auto self = shared_from_this();
    boost::asio::async_read(socket_, boost::asio::buffer(&file_size_, sizeof(file_size_)),
                            [self](const bsys::error_code& e, std::size_t transferred) {
                                self->on_file_size(e, transferred);
                            });
}

void Session::on_file_size(const bsys::error_code& ec, std::size_t) {
    if (ec) {
        abort("could not read the file size", ec);
        return;
    }

    auto refuse = [this](protocol::Status status, const std::string& detail) {
        logging::error([&](std::ostream& out) {
            out << "[SESSION " << session_id << "] Rejected: " << detail;
        });
        send_status(status);
    };

    if (file_size_ == 0) {
        refuse(protocol::Status::kRejected, "refusing an empty file");
        return;
    }
    if (file_size_ > protocol::kMaxFileSize) {
        refuse(protocol::Status::kRejected,
               "declared size " + std::to_string(file_size_) + " exceeds the limit of " +
                   std::to_string(protocol::kMaxFileSize) + " bytes");
        return;
    }

    std::string reason;
    if (!validate_filename(incoming_name_, reason)) {
        refuse(protocol::Status::kRejected, reason);
        return;
    }

    stored_file_ = std::string(kStoredFilePrefix) + incoming_name_;

    // Two clients uploading the same name would append to the same file and
    // destroy each other's data, so only one of them may hold it at a time.
    if (!server_.claim_upload(stored_file_)) {
        logging::error([&](std::ostream& out) {
            out << "[SESSION " << session_id << "] " << stored_file_
                << " is already being uploaded by another client";
        });
        send_status(protocol::Status::kBusy);
        return;
    }
    claimed_ = true;

    // How much of this file do we already hold? The handle is released before
    // anything is deleted, because Windows refuses to remove a file that is
    // still open.
    std::uint64_t existing_size = 0;
    {
        std::ifstream in(stored_file_, std::ios::binary | std::ios::ate);
        if (in) {
            const std::streampos existing = in.tellg();
            if (existing != std::streampos(-1) && existing > 0) {
                existing_size = static_cast<std::uint64_t>(existing);
            }
        }
    }

    if (existing_size > 0) {
        if (existing_size > file_size_) {
            // The partial file cannot belong to this upload: it is stale data,
            // so start over rather than appending to it.
            logging::info([&](std::ostream& out) {
                out << "[SESSION " << session_id << "] Discarding stale partial " << stored_file_
                    << " (" << existing_size << " > " << file_size_ << " bytes)";
            });
            if (std::remove(stored_file_.c_str()) != 0) {
                logging::error([&](std::ostream& out) {
                    out << "[SESSION " << session_id << "] Could not remove stale "
                        << stored_file_ << "; refused to avoid mixing two files";
                });
                send_status(protocol::Status::kError);
                return;
            }
        } else {
            resume_offset_ = existing_size;
            logging::info([&](std::ostream& out) {
                out << "[SESSION " << session_id << "] Resuming " << stored_file_ << " at "
                    << resume_offset_ << " bytes";
            });
        }
    }

    received_ = resume_offset_;
    // Logged as soon as the destination is held, so an operator can see an
    // upload start instead of only seeing it finish, and so a client can tell
    // whether its request was the one that was accepted.
    logging::info([&](std::ostream& out) {
        out << "[SESSION " << session_id << "] Accepting " << stored_file_ << " ("
            << file_size_ << " bytes, resuming at " << resume_offset_ << ")";
    });
    write_greeting();
}

// The greeting tells the client whether the request was accepted at all; only
// then does the resume offset follow.
void Session::write_greeting() {
    auto self = shared_from_this();
    unsigned char status = static_cast<unsigned char>(protocol::Status::kProceed);
    boost::asio::async_write(socket_, boost::asio::buffer(&status, sizeof(status)),
                             [self, status](const bsys::error_code& ec, std::size_t transferred) {
                                 self->on_greeting_written(ec, transferred);
                             });
}

void Session::on_greeting_written(const bsys::error_code& ec, std::size_t) {
    if (ec) {
        abort("could not acknowledge the request", ec);
        return;
    }
    write_resume_offset();
}

void Session::write_resume_offset() {
    auto self = shared_from_this();
    boost::asio::async_write(socket_, boost::asio::buffer(&resume_offset_, sizeof(resume_offset_)),
                             [self](const bsys::error_code& ec, std::size_t transferred) {
                                 self->on_resume_offset_written(ec, transferred);
                             });
}

void Session::on_resume_offset_written(const bsys::error_code& ec, std::size_t) {
    if (ec) {
        abort("could not send the resume offset", ec);
        return;
    }

    out_.open(stored_file_, std::ios::binary | std::ios::app);
    if (!out_) {
        abort("cannot open " + stored_file_ + " for writing");
        return;
    }
    if (resume_offset_ > 0) {
        out_.seekp(0, std::ios::end);
    }

    started_ = std::chrono::steady_clock::now();
    last_report_ = started_;
    last_report_bytes_ = resume_offset_;
    last_report_percent_ = -1;
    read_chunk();
}

void Session::read_chunk() {
    const std::uint64_t remaining = file_size_ - received_;
    if (remaining == 0) {
        verify_and_finish();
        return;
    }

    const auto want = static_cast<std::size_t>(
        std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(chunk_.size())));

    auto self = shared_from_this();
    boost::asio::async_read(
        socket_, boost::asio::buffer(chunk_.data(), want),
        [self](const bsys::error_code& ec, std::size_t transferred) {
            self->on_chunk(ec, transferred);
        });
}

void Session::on_chunk(const bsys::error_code& ec, std::size_t transferred) {
    if (ec) {
        // The partial data is kept: re-running the client resumes from here.
        abort("connection lost while receiving " + stored_file_, ec);
        return;
    }

    out_.write(chunk_.data(), static_cast<std::streamsize>(transferred));
    if (!out_) {
        abort("failed writing to " + stored_file_);
        return;
    }
    received_ += transferred;

    report_progress(false);
    read_chunk();
}

void Session::report_progress(bool force) {
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report_);
    if (!force && elapsed.count() < PROGRESS_INTERVAL_MS) {
        return;
    }

    const double seconds = std::max(1.0, static_cast<double>(elapsed.count()) / 1000.0);
    const double speed_MBps = ((received_ - last_report_bytes_) / 1024.0 / 1024.0) / seconds;
    const int percent = static_cast<int>((static_cast<double>(received_) / file_size_) * 100);

    if (percent != last_report_percent_) {
        std::ostringstream line;
        line << "[SESSION " << session_id << "] RECEIVING " << percent << "% | " << std::fixed
             << std::setprecision(1) << speed_MBps << " MB/s | " << received_ / 1024 / 1024 << " / "
             << file_size_ / 1024 / 1024 << " MB";
        logging::progress(line.str());
        last_report_percent_ = percent;
    }
    last_report_bytes_ = received_;
    last_report_ = now;
}

void Session::verify_and_finish() {
    // Must be flushed and closed before the digest is computed, otherwise the
    // hash would be taken over a partially written file.
    out_.flush();
    out_.close();
    if (!out_) {
        abort("failed to flush " + stored_file_);
        return;
    }

    const auto finished = std::chrono::steady_clock::now();
    const double seconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(finished - started_).count() / 1000.0;
    const std::uint64_t transferred = received_ - resume_offset_;

    logging::info([&](std::ostream& out) {
        out << "\n[SESSION " << session_id << "] " << received_ / 1000000 << " MB stored in "
            << stored_file_;
        if (seconds > 0.0 && transferred > 0) {
            out << " (" << (transferred / 1024.0 / 1024.0) / seconds << " MB/s)";
        }
    });
    logging::info([&](std::ostream& out) {
        out << "[SESSION " << session_id << "] Verifying integrity...";
    });

    std::string actual_hash;
    try {
        actual_hash = Checksum::calculate_sha256(stored_file_, true);
    } catch (const std::exception& e) {
        actual_hash.clear();
        logging::error([&](std::ostream& out) {
            out << "[SESSION " << session_id << "] Could not hash " << stored_file_ << ": "
                << e.what();
        });
    }

    if (!actual_hash.empty() && actual_hash == expected_hash_) {
        logging::info([&](std::ostream& out) {
            out << "[SESSION " << session_id << "] [SUCCESS] Hashes match (" << actual_hash << ")";
        });
        send_status(protocol::Status::kOk);
        return;
    }

    logging::error([&](std::ostream& out) {
        out << "[SESSION " << session_id << "] [ERROR] Integrity check failed: expected "
            << expected_hash_ << ", got "
            << (actual_hash.empty() ? "<could not hash>" : actual_hash);
    });

    if (std::remove(stored_file_.c_str()) == 0) {
        logging::error([&](std::ostream& out) {
            out << "[SESSION " << session_id << "] Corrupt file deleted";
        });
    } else {
        logging::error([&](std::ostream& out) {
            out << "[SESSION " << session_id << "] Could not delete " << stored_file_;
        });
    }
    send_status(protocol::Status::kChecksumMismatch);
}

void Session::send_status(protocol::Status status) {
    auto self = shared_from_this();
    unsigned char raw = static_cast<unsigned char>(status);
    boost::asio::async_write(
        socket_, boost::asio::buffer(&raw, sizeof(raw)),
        [self, raw](const bsys::error_code& ec, std::size_t transferred) {
            self->on_status_written(ec, transferred);
        });
}

void Session::on_status_written(const bsys::error_code& ec, std::size_t) {
    if (ec) {
        // The peer is already gone; there is nobody left to tell.
        logging::error([&](std::ostream& out) {
            out << "[SESSION " << session_id << "] Could not deliver the verdict: " << ec.message();
        });
    }
    release_claim();
    shutdown();
}

void Session::release_claim() {
    if (claimed_) {
        claimed_ = false;
        server_.release_upload(stored_file_);
    }
}

void Session::abort(const std::string& reason, const bsys::error_code& ec) {
    if (ec) {
        logging::error([&](std::ostream& out) {
            out << "[SESSION " << session_id << "] " << reason << ": " << ec.message();
        });
    } else {
        logging::error([&](std::ostream& out) {
            out << "[SESSION " << session_id << "] " << reason;
        });
    }

    // Anything already received stays on disk so that the transfer can resume.
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
    release_claim();
    shutdown();
}

void Session::shutdown() {
    // Close the TLS session politely so the peer can still read the status byte
    // that was just written.
    auto self = shared_from_this();
    socket_.async_shutdown([self](const bsys::error_code&) {
        bsys::error_code ignored;
        self->socket_.lowest_layer().close(ignored);
    });
}
