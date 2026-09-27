#include "client.hpp"

#include "checksum.hpp"
#include "logging.hpp"
#include "protocol.hpp"

#include <boost/system/system_error.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int PROGRESS_INTERVAL_MS = 10;

// The bytes are only on their way at this point: whether the transfer actually
// succeeded is decided by the server's status byte, which is reported later.
void print_transfer_stats(double total_seconds, std::uint64_t transferred_bytes) {
    const double mb = static_cast<double>(transferred_bytes) / 1024.0 / 1024.0;
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    if (total_seconds > 0.0) {
        out << "[INFO] Sent " << mb << " MB in " << total_seconds << " s (avg "
            << (mb / total_seconds) << " MB/s)";
    } else {
        out << "[INFO] Sent " << mb << " MB";
    }
    out << "\n[INFO] Sent in " << protocol::kChunkSize / 1024 << "KB chunks";
    logging::info([&](std::ostream& line) { line << out.str(); });
}

}  // namespace

Client::Client(boost::asio::io_context& io_context, ssl::context& ssl_ctx,
               const std::string& host, const std::string& port,
               const std::string& filepath)
    : io_context_(io_context), resolver_(io_context), socket_(io_context, ssl_ctx) {
    connect(host, port, filepath);
}

void Client::connect(const std::string& host, const std::string& port,
                     const std::string& filepath) {
    resolver_.async_resolve(
        host, port,
        [this, filepath, host, port](const boost::system::error_code& ec,
                                    tcp::resolver::results_type endpoints) {
            if (ec) {
                std::cerr << "[ERROR] Resolve failed: " << ec.message() << "\n";
                handle_error(ec);
                return;
            }

            boost::asio::async_connect(
                socket_.lowest_layer(), endpoints,
                [this, filepath, host,
                 port](const boost::system::error_code& ec, const tcp::endpoint&) {
                    if (ec) {
                        // async_connect reports a default constructed endpoint on
                        // failure, so name the target the user actually asked for.
                        std::cerr << "[ERROR] Connection to " << host << ':' << port
                                  << " failed: " << ec.message() << "\n";
                        handle_error(ec);
                        return;
                    }

                    socket_.async_handshake(
                        boost::asio::ssl::stream_base::client,
                        [this, filepath](const boost::system::error_code& ec) {
                            if (ec) {
                                std::cerr << "[ERROR] SSL handshake failed: " << ec.message() << "\n";
                                handle_error(ec);
                                return;
                            }

                            std::cout << "[INFO] Securely connected to server\n";
                            std::string path_to_send = filepath;
                            this->send_file(path_to_send);
                        });
                });
        });
}

void Client::set_server_name(const std::string& name) {
    // Without SNI a server that hosts several names cannot pick the right
    // certificate, and many hosted setups refuse the handshake outright.
    SSL_set_tlsext_host_name(socket_.native_handle(), name.c_str());
}

void Client::handle_error(const boost::system::error_code& ec) {
    std::cerr << "[ERROR] Closing connection: " << ec.message() << "\n";

    boost::system::error_code ignored_ec;
    socket_.lowest_layer().close(ignored_ec);

    // Nothing is left to do: release the event loop so the process can exit.
    io_context_.stop();
}

void Client::close() {
    boost::system::error_code ignored_ec;
    socket_.shutdown(ignored_ec);  // may fail if the handshake never completed
    socket_.lowest_layer().close(ignored_ec);
    io_context_.stop();
}

void Client::fail(const std::string& reason) {
    std::cerr << "[ERROR] " << reason << "\n";
    close();
}

void Client::send_file(std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        fail("Failed to open file: " + path);
        return;
    }

    const std::string file_hash = Checksum::calculate_sha256(path, true);
    if (file_hash.empty()) {
        fail("Failed to compute SHA-256 of " + path);
        return;
    }
    std::cout << "[INFO] SHA-256: " << file_hash << "\n";

    const std::size_t pos = path.find_last_of("/\\");
    const std::string filename = (pos == std::string::npos) ? path : path.substr(pos + 1);

    try {
        file.seekg(0, std::ios::end);
        const std::streampos end_pos = file.tellg();
        if (end_pos == std::streampos(-1)) {
            fail("Failed to determine size of " + path);
            return;
        }
        const auto file_size = static_cast<std::uint64_t>(end_pos);
        if (file_size == 0) {
            fail("Refusing to upload an empty file: " + path);
            return;
        }

        // Request: name, expected hash and size. Everything is sent up front so
        // the server can refuse the request before any payload is written.
        const auto name_len = static_cast<std::uint32_t>(filename.size());
        boost::asio::write(socket_, boost::asio::buffer(&name_len, sizeof(name_len)));
        boost::asio::write(socket_, boost::asio::buffer(filename));
        boost::asio::write(socket_, boost::asio::buffer(file_hash));
        boost::asio::write(socket_, boost::asio::buffer(&file_size, sizeof(file_size)));

        // The server either accepts the request or explains why it will not.
        unsigned char greeting = 0;
        boost::asio::read(socket_, boost::asio::buffer(&greeting, sizeof(greeting)));
        if (static_cast<protocol::Status>(greeting) != protocol::Status::kProceed) {
            const auto status = static_cast<protocol::Status>(greeting);
            std::cerr << "[ERROR] The server refused this transfer: "
                      << protocol::to_string(status);
            if (status == protocol::Status::kBusy) {
                std::cerr << " (" << filename
                          << " is already being uploaded by another client; try again shortly)";
            }
            std::cerr << "\n";
            fail("transfer refused");
            return;
        }

        std::uint64_t offset = 0;
        boost::asio::read(socket_, boost::asio::buffer(&offset, sizeof(offset)));
        std::cout << "[INFO] Resuming from offset: " << offset << " bytes\n";

        if (offset > file_size) {
            std::cerr << "[ERROR] Server holds " << offset << " bytes but " << path << " is only "
                      << file_size << " bytes.\n"
                      << "[INFO] The stale partial file was discarded; re-run the command to "
                         "upload from the start.\n";
            fail("Resuming is not possible for this file");
            return;
        }

        file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);

        std::vector<char> buffer(protocol::kChunkSize);
        const auto start_time = std::chrono::steady_clock::now();
        std::uint64_t sent = offset;
        std::uint64_t last_sent = sent;
        auto last_time = start_time;
        int last_progress = -1;

        while (sent < file_size) {
            const std::size_t wanted = static_cast<std::size_t>(
                std::min<std::uint64_t>(protocol::kChunkSize, file_size - sent));

            file.read(buffer.data(), static_cast<std::streamsize>(wanted));
            const std::streamsize bytes_read = file.gcount();
            if (bytes_read <= 0) {
                fail("Unexpected end of file after " + std::to_string(sent) + " bytes");
                return;
            }

            boost::asio::write(socket_, boost::asio::buffer(buffer.data(),
                                                             static_cast<std::size_t>(bytes_read)));
            sent += static_cast<std::uint64_t>(bytes_read);

            const auto now = std::chrono::steady_clock::now();
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time);
            if (elapsed.count() >= PROGRESS_INTERVAL_MS || sent == file_size) {
                const double speed_MBps =
                    ((sent - last_sent) / 1024.0 / 1024.0) / (elapsed.count() / 1000.0);
                const int progress = static_cast<int>((static_cast<double>(sent) / file_size) * 100);

                if (progress != last_progress || sent == file_size) {
                    // Through the logger so the bar is erased before the
                    // verdict line instead of being written over.
                    std::ostringstream bar;
                    bar << "[SENDING] " << progress << "% | " << std::fixed
                        << std::setprecision(2) << speed_MBps << " MB/s | " << sent / 1024 << " / "
                        << file_size / 1024 << " KB";
                    logging::progress(bar.str());
                    last_progress = progress;
                }

                last_sent = sent;
                last_time = now;
            }
        }

        const auto end_time = std::chrono::steady_clock::now();
        const double total_seconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count() /
            1000.0;
        logging::clear_progress();
        print_transfer_stats(total_seconds, sent - offset);

        // Wait for the server's verdict so the report reflects the real outcome.
        unsigned char status = static_cast<unsigned char>(protocol::Status::kError);
        boost::asio::read(socket_, boost::asio::buffer(&status, sizeof(status)));
        if (static_cast<protocol::Status>(status) == protocol::Status::kOk) {
            std::cout << "[SUCCESS] Server verified the file (hash matches)\n";
            succeeded_ = true;
        } else {
            std::cerr << "[ERROR] Server did not accept the file (status " << static_cast<int>(status)
                      << ")\n";
            close();
            return;
        }

    } catch (const boost::system::system_error& e) {
        std::cerr << "[ERROR] Transfer interrupted: " << e.what() << "\n";
        std::cerr << "[INFO] Re-run the same command to resume the transfer.\n";
        handle_error(e.code());
        return;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Transfer failed: " << e.what() << "\n";
        fail("transfer aborted");
        return;
    }

    close();
}
