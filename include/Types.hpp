/**
 * @file Types.hpp
 * @brief Small domain types shared by the different parts of JDN Bank.
 *
 * This file contains data that is used by more than one class. Keeping these
 * definitions in one place prevents the UI, database, and business layers
 * from inventing slightly different versions of the same idea.
 */
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace jdn {

/** A database row represented as column name -> text value. */
using Row = std::unordered_map<std::string, std::string>;

/** A collection of database rows. */
using Rows = std::vector<Row>;

/**
 * Money is stored as paise instead of double.
 *
 * For example, ₹12.50 is stored as 1250. This avoids floating-point rounding
 * errors when adding or subtracting balances.
 */
using Money = std::int64_t;

/** The three kinds of people who can use the application. */
enum class Role {
    Authority,
    Staff,
    User
};

/**
 * Information kept in memory after a successful login.
 *
 * A Session is a simple value object: it does not perform work. It only tells
 * the service layer who is currently making a request.
 */
struct Session {
    std::int64_t userId{};
    std::string login;
    std::string name;
    Role role{Role::User};
};

/** @brief Convert a Role enum into the database's uppercase text value. */
std::string roleName(Role role);

/** @brief Convert user input or database text into a Role enum. */
Role roleFromText(const std::string& value);

/** @brief Return the current local timestamp in the application's text format. */
std::string nowUtc();

/** @brief Remove whitespace from both ends of a string. */
std::string trim(const std::string& value);

/** @brief Return a lowercase copy of a string. */
std::string lower(const std::string& value);

/**
 * @brief Parse an amount such as "1250.50" into paise.
 * @throws std::invalid_argument when the input is not a positive money value.
 */
Money parseMoney(const std::string& text);

/** @brief Format paise as Indian rupees, for example ₹1,250.50. */
std::string formatMoney(Money paise);

} // namespace jdn

