/**
 * @file Security.hpp
 * @brief Password hashing and local configuration support.
 */
#pragma once

#include <cstddef>
#include <string>

namespace jdn {

/** Values read from config/jdn_bank.conf. */
struct Config {
    std::string databasePath{"data/jdn_bank.db"};
    std::string authorityRegistrationKey;

    /**
     * @brief Read a configuration file, creating it when it does not exist.
     * @throws std::runtime_error when the required authority key is missing.
     */
    static Config loadOrCreate(const std::string& path);
};

/**
 * @brief Small security service used by the application.
 *
 * The class owns configuration and exposes password-related operations. It is
 * intentionally separate from BankService so banking rules do not need to
 * know how password hashes or random salts are produced.
 */
class Security {
public:
    explicit Security(Config config);

    /** @brief Return the configuration owned by this object. */
    const Config& config() const { return config_; }

    /** @brief Hash a password together with its per-user salt. */
    std::string hashPassword(const std::string& password,
                             const std::string& salt) const;

    /** @brief Create random hexadecimal text containing the requested bytes. */
    std::string randomHex(std::size_t bytes) const;

private:
    Config config_;
};

} // namespace jdn

