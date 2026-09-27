#include "client.hpp"
#include "server.hpp"

#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/ssl.hpp>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr unsigned short DEFAULT_PORT = 9000;
constexpr const char* DEFAULT_HOST = "127.0.0.1";

// Rejects anything that is not a plain decimal port number in 1-65535, so that
// a typo such as "70000" is reported instead of being silently wrapped around
// by the unsigned short conversion.
bool parse_port(const std::string& text, unsigned short& port) {
    if (text.empty() || text.size() > 5) {
        return false;
    }
    unsigned long value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<unsigned long>(c - '0');
    }
    if (value == 0 || value > 65535) {
        return false;
    }
    port = static_cast<unsigned short>(value);
    return true;
}

void print_usage(const char* program) {
    std::cerr << "Usage:\n"
              << "  " << program << " server [port]\n"
              << "  " << program << " client <filepath> [host] [port] [--ca <file> | --insecure]\n"
              << "\n"
              << "  --ca <file>  verify the server certificate against this CA file\n"
              << "  --insecure   skip certificate verification (never use this on a real network)\n";
}

// The directory the running executable lives in. Assets are looked for here
// first, so that the client keeps working when it is launched from somewhere
// else that happens to contain a tls_key directory of its own.
std::string executable_directory() {
#ifdef _WIN32
    char buffer[MAX_PATH] = {};
    const DWORD length = ::GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    const std::string full(buffer, length);
#elif defined(__linux__)
    char buffer[4096] = {};
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) {
        return {};
    }
    const std::string full(buffer, static_cast<std::size_t>(length));
#else
    return {};
#endif
    const std::size_t slash = full.find_last_of("/\\");
    if (slash == std::string::npos) {
        return {};
    }
    return full.substr(0, slash);
}

bool file_is_readable(const std::string& path) {
    std::ifstream probe(path, std::ios::binary);
    return probe.good();
}

// Looks next to the executable first, then in the working directory, so the
// build's own copy of the certificate wins over an unrelated one.
std::string locate_tls_asset(const std::string& filename) {
    const std::string exe_dir = executable_directory();
    if (!exe_dir.empty()) {
        const std::string next_to_exe = exe_dir + "/" + filename;
        const std::string next_to_exe_in_tls_key = exe_dir + "/tls_key/" + filename;
        if (file_is_readable(next_to_exe)) {
            return next_to_exe;
        }
        if (file_is_readable(next_to_exe_in_tls_key)) {
            return next_to_exe_in_tls_key;
        }
    }
    const std::string candidates[] = {filename, "tls_key/" + filename};
    for (const std::string& path : candidates) {
        if (file_is_readable(path)) {
            return path;
        }
    }
    return {};
}

// Accepts the certificate only when the chain is valid *and* the certificate
// names the host we asked for. Boost.Asio's own host_name_verification does not
// understand IP literals, which is what most callers use here.
std::function<bool(bool, boost::asio::ssl::verify_context&)> make_host_check(
    const std::string& host) {
    return [host](bool preverified, boost::asio::ssl::verify_context& ctx) {
        // Never override a chain that failed validation: returning true here
        // would let any certificate through as long as it names our host.
        if (!preverified) {
            return false;
        }
        X509_STORE_CTX* store = ctx.native_handle();
        if (store == nullptr) {
            return false;
        }
        // Only the certificate at the end of the chain identifies the server.
        if (X509_STORE_CTX_get_error_depth(store) > 0) {
            return true;
        }
        X509* cert = X509_STORE_CTX_get_current_cert(store);
        if (cert == nullptr) {
            return false;
        }
        if (X509_check_ip_asc(cert, host.c_str(), 0) == 1) {
            return true;
        }
        return X509_check_host(cert, host.c_str(), host.size(),
                              X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS, nullptr) == 1;
    };
}

// Drives the event loop from a fixed number of threads so that slow sessions
// cannot hold up the others. Every session keeps at most one operation in
// flight, so the handlers of a single session never run concurrently.
void run_event_loop(boost::asio::io_context& io, unsigned thread_count) {
    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (unsigned i = 0; i < thread_count; ++i) {
        workers.emplace_back([&io] { io.run(); });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
}

unsigned server_thread_count() {
    const unsigned hardware = std::thread::hardware_concurrency();
    if (hardware <= 2) {
        return 2;
    }
    return std::min(4u, hardware);
}

int run_server(boost::asio::io_context& io, unsigned short port) {
    // The Server object owns the acceptor, so it has to outlive io.run().
    Server server(io, port);

    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&server, &io](const boost::system::error_code& ec, int signal) {
        if (ec) {
            return;
        }
        std::cerr << "\n[SERVER] Signal " << signal << " received, shutting down\n";
        server.stop();
        io.stop();
    });

    run_event_loop(io, server_thread_count());
    return 0;
}

int run_client(boost::asio::io_context& io, const std::string& filepath,
               const std::string& host, const std::string& port,
               const std::string& ca_file, bool insecure) {
    namespace ssl = boost::asio::ssl;
    ssl::context ssl_ctx(ssl::context::tls_client);

    if (insecure) {
        std::cerr << "[WARN] Certificate verification is disabled (--insecure).\n";
        ssl_ctx.set_verify_mode(ssl::verify_none);
    } else {
        // Without an explicit CA, fall back to the certificate shipped next to
        // the executable so that the development setup verifies by default.
        const std::string ca = ca_file.empty() ? locate_tls_asset("server.crt") : ca_file;
        if (ca.empty()) {
            std::cerr << "[ERROR] No CA certificate found. Pass --ca <file>, or use "
                         "--insecure to skip verification.\n";
            return 1;
        }

        boost::system::error_code ec;
        ssl_ctx.load_verify_file(ca, ec);
        if (ec) {
            std::cerr << "[ERROR] Could not load the CA certificate " << ca << ": " << ec.message()
                      << "\n";
            return 1;
        }
        ssl_ctx.set_verify_mode(ssl::verify_peer);
        ssl_ctx.set_verify_callback(make_host_check(host), ec);
        if (ec) {
            std::cerr << "[ERROR] Could not install the host check: " << ec.message() << "\n";
            return 1;
        }
        // The resolved path is printed because "certificate verify failed" on
        // its own says nothing about *which* certificate was trusted, and the
        // wrong one is a common and confusing mistake.
        std::cerr << "[INFO] Verifying the server certificate against " << ca << "\n";
    }

    // The Client object owns the socket, so it has to outlive io.run().
    Client client(io, ssl_ctx, host, port, filepath);
    client.set_server_name(host);
    run_event_loop(io, 1);
    return client.succeeded() ? 0 : 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argc > 0 ? argv[0] : "FTP");
        return 1;
    }

    // The server's progress and status lines are its only visibility into what
    // is happening, and the output is usually redirected to a file, where
    // std::cout would otherwise be block buffered and only appear in bursts.
    std::cout << std::unitbuf;

    const std::string mode = argv[1];
    boost::asio::io_context io;

    try {
        if (mode == "server") {
            if (argc > 3) {
                print_usage(argv[0]);
                return 1;
            }
            unsigned short port = DEFAULT_PORT;
            if (argc == 3 && !parse_port(argv[2], port)) {
                std::cerr << "[ERROR] Invalid port: '" << argv[2]
                          << "'. Expected a number between 1 and 65535.\n";
                return 1;
            }
            return run_server(io, port);

        } else if (mode == "client") {
            if (argc < 3) {
                print_usage(argv[0]);
                return 1;
            }

            const std::string filepath = argv[2];
            std::string host = DEFAULT_HOST;
            std::string port;
            std::string ca_file;
            bool insecure = false;

            // Positional host/port may be followed by the verification options.
            std::vector<std::string> positional;
            for (int i = 3; i < argc; ++i) {
                const std::string arg = argv[i];
                if (arg == "--ca") {
                    if (i + 1 >= argc) {
                        std::cerr << "[ERROR] --ca needs a file path\n";
                        return 1;
                    }
                    ca_file = argv[++i];
                } else if (arg == "--insecure" || arg == "-k") {
                    insecure = true;
                } else if (!arg.empty() && arg[0] == '-') {
                    std::cerr << "[ERROR] Unknown option: " << arg << "\n";
                    print_usage(argv[0]);
                    return 1;
                } else {
                    positional.push_back(arg);
                }
            }

            if (positional.size() > 2) {
                print_usage(argv[0]);
                return 1;
            }
            if (!positional.empty()) {
                host = positional[0];
            }
            if (positional.size() > 1) {
                port = positional[1];
            }
            if (port.empty()) {
                port = std::to_string(DEFAULT_PORT);
            }
            unsigned short parsed_port = 0;
            if (!parse_port(port, parsed_port)) {
                std::cerr << "[ERROR] Invalid port: '" << port
                          << "'. Expected a number between 1 and 65535.\n";
                return 1;
            }
            if (ca_file.empty() && !insecure) {
                // Nothing to verify against: fail loudly instead of silently
                // accepting whatever the server presents.
                if (locate_tls_asset("server.crt").empty()) {
                    std::cerr << "[ERROR] No CA certificate found. Pass --ca <file>, or use "
                                 "--insecure to skip verification.\n";
                    return 1;
                }
            }
            return run_client(io, filepath, host, port, ca_file, insecure);
        }

        print_usage(argv[0]);
        return 1;

    } catch (const std::exception& e) {
        std::cerr << "[FATAL] " << e.what() << "\n";
        return 1;
    }
}
