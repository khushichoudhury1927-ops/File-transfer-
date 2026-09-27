#include "server.hpp"

#include "logging.hpp"
#include "serverSession.hpp"

#include <boost/system/system_error.hpp>
#include <exception>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>

namespace {

// Looks for a TLS asset next to the working directory first, then in ./tls_key.
std::string locate_tls_asset(const std::string& filename) {
    const std::string candidates[] = {filename, "tls_key/" + filename};
    for (const std::string& path : candidates) {
        std::ifstream probe(path, std::ios::binary);
        if (probe.good()) {
            return path;
        }
    }
    return {};
}

}  // namespace

Server::Server(boost::asio::io_context& io, unsigned short port)
    : io_context_(io),
      ctx(boost::asio::ssl::context::tls_server),
      acceptConnect(io, tcp::endpoint(tcp::v4(), port)) {
    boost::system::error_code ec;

    const std::string cert = locate_tls_asset("server.crt");
    const std::string key = locate_tls_asset("server.key");

    if (cert.empty() || key.empty()) {
        throw std::runtime_error(
            "TLS assets not found. Expected server.crt and server.key in the working "
            "directory or in ./tls_key (generate them with: openssl req -x509 -newkey "
            "rsa:2048 -nodes -keyout tls_key/server.key -out tls_key/server.crt -days 365 "
            "-subj \"/CN=localhost\" -addext \"subjectAltName=DNS:localhost,IP:127.0.0.1\")");
    }

    ctx.set_options(boost::asio::ssl::context::default_workarounds |
                    boost::asio::ssl::context::no_sslv2 |
                    boost::asio::ssl::context::no_sslv3 |
                    boost::asio::ssl::context::single_dh_use);

    // These throw boost::system::system_error when the file cannot be used.
    ctx.use_certificate_chain_file(cert);
    ctx.use_private_key_file(key, boost::asio::ssl::context::pem);

    acceptConnect.listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) {
        throw boost::system::system_error(ec, "listen");
    }

    logging::info([&](std::ostream& out) {
        out << "[SERVER] Listening on port " << port << " (TLS enabled)";
    });
    accept_connection();
}

Server::~Server() {
    stop();
}

void Server::stop() {
    boost::system::error_code ignored;
    acceptConnect.close(ignored);
}

bool Server::claim_upload(const std::string& stored_name) {
    const std::lock_guard<std::mutex> lock(uploads_mutex_);
    return active_uploads_.insert(stored_name).second;
}

void Server::release_upload(const std::string& stored_name) {
    const std::lock_guard<std::mutex> lock(uploads_mutex_);
    active_uploads_.erase(stored_name);
}

std::size_t Server::active_uploads() const {
    const std::lock_guard<std::mutex> lock(uploads_mutex_);
    return active_uploads_.size();
}

void Server::accept_connection() {
    acceptConnect.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
        if (!ec) {
            boost::system::error_code endpoint_ec;
            const auto endpoint = socket.remote_endpoint(endpoint_ec);
            if (!endpoint_ec) {
                logging::info([&](std::ostream& out) {
                    out << "[SERVER] New client: " << endpoint.address().to_string() << ':'
                        << endpoint.port();
                });
            }
            // The server outlives every session: main keeps it alive for as long
            // as io_context::run() is in progress.
            std::make_shared<Session>(std::move(socket), ctx, *this)->start_session();
        } else if (ec == boost::asio::error::operation_aborted) {
            return;  // the server is shutting down
        } else {
            logging::error([&](std::ostream& out) {
                out << "[SERVER] Accept error: " << ec.message();
            });
        }
        // Restart the accept from the io_context thread so the handler chain
        // cannot grow the stack.
        boost::asio::post(io_context_, [this] { accept_connection(); });
    });
}
