#ifndef SERVER_HPP
#define SERVER_HPP

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <mutex>
#include <string>
#include <unordered_set>

using boost::asio::ip::tcp;

class Server {
public:
    Server(boost::asio::io_context& io_context, unsigned short port);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Stops accepting so that io_context::run() can return.
    void stop();

    // Claims `stored_name` for the duration of one upload. Returns false when
    // another session already holds it, which keeps two clients from appending
    // to the same file. The claim must be handed back with release_upload().
    bool claim_upload(const std::string& stored_name);
    void release_upload(const std::string& stored_name);

    std::size_t active_uploads() const;

private:
    void accept_connection();

    boost::asio::io_context& io_context_;
    boost::asio::ssl::context ctx;
    tcp::acceptor acceptConnect;

    mutable std::mutex uploads_mutex_;
    std::unordered_set<std::string> active_uploads_;
};

#endif // SERVER_HPP
