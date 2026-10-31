/**
 * @file Types.cpp
 * @brief Implementations of shared value conversions and formatting helpers.
 */
#include "Types.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace jdn {
namespace {

/** @brief Test whether a character is an ASCII digit. */
bool isDigit(char character) {
    return std::isdigit(static_cast<unsigned char>(character)) != 0;
}

} // namespace

std::string roleName(Role role) {
    switch (role) {
    case Role::Authority:
        return "AUTHORITY";
    case Role::Staff:
        return "STAFF";
    case Role::User:
        return "USER";
    }

    // This keeps the compiler happy if another enum value is added later.
    return "USER";
}

Role roleFromText(const std::string& value) {
    const std::string normalised = lower(value);
    if (normalised == "authority") {
        return Role::Authority;
    }
    if (normalised == "staff") {
        return Role::Staff;
    }
    return Role::User;
}

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }

    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lower(const std::string& value) {
    std::string result = value;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return result;
}

std::string nowUtc() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm calendarTime{};

#if defined(_WIN32)
    localtime_s(&calendarTime, &time);
#else
    localtime_r(&time, &calendarTime);
#endif

    std::ostringstream output;
    output << std::put_time(&calendarTime, "%Y-%m-%d %H:%M:%S");
    return output.str();
}

Money parseMoney(const std::string& text) {
    const std::string value = trim(text);
    if (value.empty() || value.front() == '-') {
        throw std::invalid_argument("Enter a positive amount.");
    }

    const auto decimalPoint = value.find('.');
    std::string whole = decimalPoint == std::string::npos
                            ? value
                            : value.substr(0, decimalPoint);
    std::string fraction = decimalPoint == std::string::npos
                               ? ""
                               : value.substr(decimalPoint + 1);

    if (whole.empty()) {
        whole = "0";
    }

    const bool validWhole = std::all_of(whole.begin(), whole.end(), isDigit);
    const bool validFraction = std::all_of(fraction.begin(), fraction.end(), isDigit);
    if (fraction.size() > 2 || !validWhole || !validFraction) {
        throw std::invalid_argument("Use an amount such as 1250.50.");
    }

    while (fraction.size() < 2) {
        fraction.push_back('0');
    }

    return std::stoll(whole) * 100 + std::stoll(fraction);
}

std::string formatMoney(Money paise) {
    std::ostringstream output;
    const bool negative = paise < 0;
    if (negative) {
        paise = -paise;
    }

    std::string whole = std::to_string(paise / 100);
    if (whole.size() > 3) {
        for (std::size_t position = whole.size() - 3; position > 0;) {
            whole.insert(position, ",");
            if (position <= 2) {
                break;
            }
            position -= 2;
        }
    }

    output << (negative ? "-₹" : "₹") << whole << '.'
           << std::setw(2) << std::setfill('0') << paise % 100;
    return output.str();
}

} // namespace jdn

