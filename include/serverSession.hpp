#ifndef SERVER_SESSION_HPP
#define SERVER_SESSION_HPP

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "protocol.hpp"
#include "server.hpp"

using boost::asio::ip::tcp;
namespace ssl = boost::asio::ssl;
namespace bsys = boost::system;

// One client connection. Every step of the transfer is asynchronous and at most
// one operation is ever outstanding, so a session never blocks a worker thread
// and its own handlers can never run concurrently with each other.
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, ssl::context& ctx, Server& server);

    void start_session();

    // Monotonic id, shared by every session instance.
    static std::atomic<unsigned int> session_count;

private:
    void read_name_length();
    void on_name_length(const bsys::error_code& ec, std::size_t transferred);
    void on_filename(const bsys::error_code& ec, std::size_t transferred);
    void on_hash(const bsys::error_code& ec, std::size_t transferred);
    void on_file_size(const bsys::error_code& ec, std::size_t transferred);

    void write_greeting();
    void on_greeting_written(const bsys::error_code& ec, std::size_t transferred);
    void write_resume_offset();
    void on_resume_offset_written(const bsys::error_code& ec, std::size_t transferred);

    void read_chunk();
    void on_chunk(const bsys::error_code& ec, std::size_t transferred);

    void verify_and_finish();
    void send_status(protocol::Status status);
    void on_status_written(const bsys::error_code& ec, std::size_t transferred);

    // Ends the session: releases the file claim, closes the file and the socket.
    void abort(const std::string& reason, const bsys::error_code& ec = bsys::error_code());
    void release_claim();
    void shutdown();
    void report_progress(bool force);

    // Validates a client supplied name. Returns false when it is not a single,
    // harmless file name; the reason is written to `reason`.
    static bool validate_filename(const std::string& raw, std::string& reason);

    ssl::stream<tcp::socket> socket_;
    Server& server_;
    const unsigned int session_id;

    // Request header, filled in as it arrives.
    std::uint32_t name_length_ = 0;
    std::string incoming_name_;
    std::string expected_hash_;
    std::uint64_t file_size_ = 0;
    std::uint64_t resume_offset_ = 0;
    std::uint64_t received_ = 0;

    std::string stored_file_;
    bool claimed_ = false;
    std::ofstream out_;
    std::vector<char> chunk_;

    // Progress reporting.
    std::chrono::steady_clock::time_point started_;
    std::chrono::steady_clock::time_point last_report_;
    std::uint64_t last_report_bytes_ = 0;
    int last_report_percent_ = -1;
};

#endif // SERVER_SESSION_HPP
