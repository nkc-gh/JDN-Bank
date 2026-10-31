/**
 * @file Security.cpp
 * @brief Configuration loading, random salts, and the local SHA-256 helper.
 */
#include "Security.hpp"
#include "Types.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace jdn {
namespace {

// These are the standard SHA-256 round constants.
constexpr std::array<std::uint32_t, 64> SHA256_CONSTANTS = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

/** @brief Rotate a 32-bit value to the right. */
std::uint32_t rotateRight(std::uint32_t value, int amount) {
    return (value >> amount) | (value << (32 - amount));
}

/**
 * @brief Produce a SHA-256 digest for one string.
 *
 * This is kept local to this source file because the rest of the application
 * only needs the higher-level Security::hashPassword function.
 */
std::string sha256(const std::string& input) {
    std::vector<unsigned char> message(input.begin(), input.end());
    const std::uint64_t bitLength =
        static_cast<std::uint64_t>(message.size()) * 8U;

    // SHA-256 pads the message with one 1 bit, zeroes, and the original length.
    message.push_back(0x80);
    while (message.size() % 64 != 56) {
        message.push_back(0);
    }
    for (int shift = 7; shift >= 0; --shift) {
        message.push_back(static_cast<unsigned char>(bitLength >> (shift * 8)));
    }

    std::uint32_t hash[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    // Process each 512-bit block.
    for (std::size_t offset = 0; offset < message.size(); offset += 64) {
        std::uint32_t words[64]{};

        // Convert the first 16 big-endian 4-byte groups into words.
        for (int index = 0; index < 16; ++index) {
            const std::size_t position = offset + static_cast<std::size_t>(index) * 4;
            words[index] =
                (static_cast<std::uint32_t>(message[position]) << 24) |
                (static_cast<std::uint32_t>(message[position + 1]) << 16) |
                (static_cast<std::uint32_t>(message[position + 2]) << 8) |
                static_cast<std::uint32_t>(message[position + 3]);
        }

        // Expand the 16 input words into the 64 words used by the rounds.
        for (int index = 16; index < 64; ++index) {
            const auto smallSigma0 =
                rotateRight(words[index - 15], 7) ^
                rotateRight(words[index - 15], 18) ^
                (words[index - 15] >> 3);
            const auto smallSigma1 =
                rotateRight(words[index - 2], 17) ^
                rotateRight(words[index - 2], 19) ^
                (words[index - 2] >> 10);
            words[index] = words[index - 16] + smallSigma0 +
                           words[index - 7] + smallSigma1;
        }

        std::uint32_t a = hash[0];
        std::uint32_t b = hash[1];
        std::uint32_t c = hash[2];
        std::uint32_t d = hash[3];
        std::uint32_t e = hash[4];
        std::uint32_t f = hash[5];
        std::uint32_t g = hash[6];
        std::uint32_t h = hash[7];

        for (int index = 0; index < 64; ++index) {
            const auto bigSigma1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^
                                   rotateRight(e, 25);
            const auto choose = (e & f) ^ ((~e) & g);
            const auto temporary1 = h + bigSigma1 + choose +
                                    SHA256_CONSTANTS[static_cast<std::size_t>(index)] +
                                    words[index];
            const auto bigSigma0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^
                                   rotateRight(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporary2 = bigSigma0 + majority;

            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }

        hash[0] += a;
        hash[1] += b;
        hash[2] += c;
        hash[3] += d;
        hash[4] += e;
        hash[5] += f;
        hash[6] += g;
        hash[7] += h;
    }

    std::ostringstream output;
    for (const auto value : hash) {
        output << std::hex << std::setw(8) << std::setfill('0') << value;
    }
    return output.str();
}

} // namespace

Config Config::loadOrCreate(const std::string& path) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    if (!std::filesystem::exists(path)) {
        std::random_device randomDevice;
        std::mt19937_64 generator(randomDevice());
        auto makeKey = [&generator]() {
            std::ostringstream output;
            for (int index = 0; index < 4; ++index) {
                output << std::hex << std::setw(16) << std::setfill('0')
                       << generator();
            }
            return output.str();
        };

        std::ofstream output(path);
        output << "# JDN Bank local configuration. Keep this file private.\n"
               << "database_path=data/jdn_bank.db\n"
               << "authority_registration_key=" << makeKey() << "\n";
    }

    Config config;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }

        const auto key = trim(line.substr(0, equals));
        const auto value = trim(line.substr(equals + 1));
        if (key == "database_path") {
            config.databasePath = value;
        } else if (key == "authority_registration_key") {
            config.authorityRegistrationKey = value;
        }
    }

    if (config.authorityRegistrationKey.empty()) {
        throw std::runtime_error(
            "Configuration is missing the authority registration key.");
    }
    return config;
}

Security::Security(Config config) : config_(std::move(config)) {}

std::string Security::randomHex(std::size_t bytes) const {
    std::random_device randomDevice;
    std::ostringstream output;
    for (std::size_t index = 0; index < bytes; ++index) {
        output << std::hex << std::setw(2) << std::setfill('0')
               << (randomDevice() & 0xffU);
    }
    return output.str();
}

std::string Security::hashPassword(const std::string& password,
                                   const std::string& salt) const {
    return sha256("JDN-BANK-PASSWORD-v1|" + salt + "|" + password);
}

} // namespace jdn

