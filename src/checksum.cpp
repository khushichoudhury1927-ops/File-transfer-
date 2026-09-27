#include "checksum.hpp"

#include "logging.hpp"

#include <openssl/evp.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kReadBufferSize = 65536;  // 64 KB

}  // namespace

std::string Checksum::calculate_sha256(const std::string& filepath, bool show_progress) {
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) {
        logging::error([&](std::ostream& out) {
            out << "[CHECKSUM] Cannot open " << filepath;
        });
        return "";
    }

    const auto total_size = static_cast<std::uint64_t>(file.tellg());
    if (total_size == static_cast<std::uint64_t>(-1)) {
        logging::error([&](std::ostream& out) {
            out << "[CHECKSUM] Cannot determine size of " << filepath;
        });
        return "";
    }
    file.seekg(0, std::ios::beg);

    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if (mdctx == nullptr) {
        logging::error([&](std::ostream& out) { out << "[CHECKSUM] EVP_MD_CTX_new failed"; });
        return "";
    }

    if (EVP_DigestInit_ex(mdctx, EVP_sha256(), nullptr) != 1) {
        logging::error([&](std::ostream& out) { out << "[CHECKSUM] EVP_DigestInit_ex failed"; });
        EVP_MD_CTX_free(mdctx);
        return "";
    }

    std::vector<char> buffer(kReadBufferSize);
    std::uint64_t bytes_processed = 0;
    int last_progress = -1;

    while (file.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || file.gcount() > 0) {
        const std::streamsize bytes = file.gcount();
        if (bytes > 0 &&
            EVP_DigestUpdate(mdctx, buffer.data(), static_cast<std::size_t>(bytes)) != 1) {
            logging::error([&](std::ostream& out) { out << "[CHECKSUM] EVP_DigestUpdate failed"; });
            EVP_MD_CTX_free(mdctx);
            logging::clear_progress();
            return "";
        }

        bytes_processed += static_cast<std::uint64_t>(bytes);

        if (show_progress) {
            const int progress =
                total_size ? static_cast<int>((static_cast<double>(bytes_processed) / total_size) * 100)
                           : 100;
            if (progress != last_progress) {
                // Goes through the logger because another session may be
                // writing a line at this instant.
                std::ostringstream line;
                line << "[HASHING] " << progress << "%";
                logging::progress(line.str());
                last_progress = progress;
            }
        }
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
    unsigned int hash_length = 0;
    const int finalised = EVP_DigestFinal_ex(mdctx, hash.data(), &hash_length);
    EVP_MD_CTX_free(mdctx);

    if (show_progress) {
        // Take the bar down so the result line is not written over it.
        logging::clear_progress();
    }

    if (finalised != 1) {
        logging::error([&](std::ostream& out) { out << "[CHECKSUM] EVP_DigestFinal_ex failed"; });
        return "";
    }

    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < hash_length; ++i) {
        ss << std::setw(2) << static_cast<int>(hash[i]);
    }
    return ss.str();
}
