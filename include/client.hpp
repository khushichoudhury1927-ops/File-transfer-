#ifndef CLIENT_HPP
#define CLIENT_HPP

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <string>

using boost::asio::ip::tcp;
namespace ssl = boost::asio::ssl;

class Client {
public:
    Client(boost::asio::io_context& io_context, ssl::context& ssl_ctx,
           const std::string& host, const std::string& port, const std::string& filepath);

    // Uploads `filepath` once the TLS connection to host:port is established.
    void connect(const std::string& host, const std::string& port, const std::string& filepath);

    // Adds the server name to the TLS handshake (SNI). Must be called before
    // the connection is established.
    void set_server_name(const std::string& name);

    // Streams a local file to the server in chunks, resuming from the offset
    // the server asks for.
    void send_file(std::string& path);

    void handle_error(const boost::system::error_code& ec);

    void close();

    // Reports a local failure (bad path, unreadable file, ...) and releases the
    // event loop so the process can exit.
    void fail(const std::string& reason);

    // True once the server has confirmed that the file arrived intact.
    bool succeeded() const { return succeeded_; }

private:
    boost::asio::io_context& io_context_;
    tcp::resolver resolver_;
    ssl::stream<tcp::socket> socket_;
    bool succeeded_ = false;
};

#endif // CLIENT_HPP
