#ifndef CHECKSUM_HPP
#define CHECKSUM_HPP

#include <string>

class Checksum {
public:
    // to returns the hex string of the SHA-256 hash
    static std::string calculate_sha256(const std::string& filepath, bool show_progress = false);
};

#endif