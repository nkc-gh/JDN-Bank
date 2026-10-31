/**
 * @file Database.cpp
 * @brief SQLite connection, queries, transactions, and schema migration.
 */
#include "Database.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

// The project uses the small SQLite C API directly instead of requiring the
// full sqlite3.h header at compile time.
extern "C" {
struct sqlite3;
using sqlite3_callback = int (*)(void*, int, char**, char**);
int sqlite3_open_v2(const char*, sqlite3**, int, const char*);
int sqlite3_close(sqlite3*);
int sqlite3_exec(sqlite3*, const char*, sqlite3_callback, void*, char**);
const char* sqlite3_errmsg(sqlite3*);
void sqlite3_free(void*);
long long sqlite3_last_insert_rowid(sqlite3*);
}

namespace jdn {
namespace {

constexpr int SQLITE_OK = 0;
constexpr int SQLITE_OPEN_READWRITE = 0x00000002;
constexpr int SQLITE_OPEN_CREATE = 0x00000004;

/** @brief Convert one SQLite callback result into a Row. */
int collectRows(void* target, int columnCount, char** values, char** names) {
    auto& rows = *static_cast<Rows*>(target);
    Row row;

    for (int column = 0; column < columnCount; ++column) {
        row[names[column]] = values[column] == nullptr ? "" : values[column];
    }

    rows.push_back(std::move(row));
    return 0;
}

} // namespace

Database::Database(const std::string& path) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    const int result = sqlite3_open_v2(
        path.c_str(), &handle_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
        nullptr);
    if (result != SQLITE_OK) {
        const std::string message =
            handle_ == nullptr ? "unknown error" : sqlite3_errmsg(handle_);
        throw std::runtime_error("Cannot open SQLite database: " + message);
    }

    // Foreign keys, WAL mode, and a short busy timeout make local multi-terminal
    // use safer without changing the public application API.
    exec("PRAGMA foreign_keys = ON; "
         "PRAGMA journal_mode = WAL; "
         "PRAGMA busy_timeout = 5000;");
}

Database::~Database() {
    if (handle_ != nullptr) {
        sqlite3_close(handle_);
    }
}

void Database::exec(const std::string& sql) const {
    char* error = nullptr;
    if (sqlite3_exec(handle_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message =
            error == nullptr ? sqlite3_errmsg(handle_) : error;
        if (error != nullptr) {
            sqlite3_free(error);
        }
        throw std::runtime_error("Database error: " + message);
    }
}

Rows Database::query(const std::string& sql) const {
    Rows rows;
    char* error = nullptr;

    if (sqlite3_exec(handle_, sql.c_str(), collectRows, &rows, &error) != SQLITE_OK) {
        const std::string message =
            error == nullptr ? sqlite3_errmsg(handle_) : error;
        if (error != nullptr) {
            sqlite3_free(error);
        }
        throw std::runtime_error("Database query error: " + message);
    }

    return rows;
}

std::int64_t Database::scalarInt(const std::string& sql,
                                 std::int64_t fallback) const {
    const Rows rows = query(sql);
    if (rows.empty() || rows.front().empty()) {
        return fallback;
    }

    try {
        return std::stoll(rows.front().begin()->second);
    } catch (...) {
        return fallback;
    }
}

std::string Database::scalarText(const std::string& sql,
                                 const std::string& fallback) const {
    const Rows rows = query(sql);
    if (rows.empty() || rows.front().empty()) {
        return fallback;
    }
    return rows.front().begin()->second;
}

std::int64_t Database::lastInsertId() const {
    return sqlite3_last_insert_rowid(handle_);
}

void Database::begin() {
    exec("BEGIN IMMEDIATE;");
}

void Database::commit() {
    exec("COMMIT;");
}

void Database::rollback() noexcept {
    try {
        exec("ROLLBACK;");
    } catch (...) {
        // A rollback must not hide the original exception.
    }
}

std::string Database::quote(const std::string& value) {
    std::string escaped = "'";
    for (const char character : value) {
        escaped += character;
        if (character == '\'') {
            // SQLite escapes a quote inside a string by doubling it.
            escaped += '\'';
        }
    }
    escaped += "'";
    return escaped;
}

Transaction::Transaction(Database& db) : db_(db) {
    db_.begin();
}

Transaction::~Transaction() {
    if (!committed_) {
        db_.rollback();
    }
}

void Transaction::commit() {
    db_.commit();
    committed_ = true;
}

void Database::migrate() {
    // CREATE TABLE IF NOT EXISTS makes this safe to run on every startup.
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS users (
 user_id INTEGER PRIMARY KEY, login TEXT UNIQUE NOT NULL, role TEXT NOT NULL, name TEXT NOT NULL,
 email TEXT DEFAULT '', phone TEXT DEFAULT '', dob TEXT DEFAULT '', address TEXT DEFAULT '', password_hash TEXT NOT NULL, password_salt TEXT NOT NULL,
 status TEXT NOT NULL DEFAULT 'ACTIVE', failed_attempts INTEGER NOT NULL DEFAULT 0, locked_until TEXT,
 credit_score INTEGER NOT NULL DEFAULT 650, created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS customer_profiles (
 user_id INTEGER PRIMARY KEY REFERENCES users(user_id), aadhaar_enc TEXT NOT NULL, pan_enc TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS accounts (
 account_id INTEGER PRIMARY KEY, account_number TEXT UNIQUE NOT NULL, user_id INTEGER REFERENCES users(user_id),
 account_type TEXT NOT NULL DEFAULT 'SAVINGS', status TEXT NOT NULL DEFAULT 'ACTIVE', frozen INTEGER NOT NULL DEFAULT 0,
 min_balance INTEGER NOT NULL DEFAULT 100000, interest_bps INTEGER NOT NULL DEFAULT 350,
 last_interest_at TEXT NOT NULL, created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS approval_requests (
 request_id INTEGER PRIMARY KEY, request_type TEXT NOT NULL, requester_name TEXT DEFAULT '', requester_login TEXT DEFAULT '',
 requester_password_hash TEXT DEFAULT '', requester_password_salt TEXT DEFAULT '', requester_email TEXT DEFAULT '',
 requester_phone TEXT DEFAULT '', requester_dob TEXT DEFAULT '', requester_address TEXT DEFAULT '', aadhaar_enc TEXT DEFAULT '', pan_enc TEXT DEFAULT '', opening_balance INTEGER DEFAULT 0,
 subject_user_id INTEGER, assigned_staff_id INTEGER, amount INTEGER DEFAULT 0, metadata TEXT DEFAULT '',
 status TEXT NOT NULL DEFAULT 'PENDING', staff_reviewer_id INTEGER, authority_reviewer_id INTEGER,
 created_at TEXT NOT NULL, updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS bank_transactions (
 transaction_id INTEGER PRIMARY KEY, transaction_ref TEXT UNIQUE NOT NULL, transaction_type TEXT NOT NULL,
 status TEXT NOT NULL, initiated_by INTEGER, memo TEXT DEFAULT '', created_at TEXT NOT NULL,
 previous_hash TEXT NOT NULL, current_hash TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS ledger_entries (
 entry_id INTEGER PRIMARY KEY, transaction_id INTEGER NOT NULL REFERENCES bank_transactions(transaction_id),
 account_id INTEGER NOT NULL REFERENCES accounts(account_id), entry_side TEXT NOT NULL CHECK(entry_side IN ('DEBIT','CREDIT')),
 amount INTEGER NOT NULL CHECK(amount > 0), created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS loans (
 loan_id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(user_id), amount INTEGER NOT NULL,
 purpose TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'PENDING', request_id INTEGER, disbursed_at TEXT,
 outstanding INTEGER NOT NULL DEFAULT 0, term_months INTEGER NOT NULL DEFAULT 12,
 monthly_emi INTEGER NOT NULL DEFAULT 0, next_due_date TEXT, created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS cheques (
 cheque_id INTEGER PRIMARY KEY, account_id INTEGER NOT NULL REFERENCES accounts(account_id), cheque_number TEXT UNIQUE NOT NULL,
 status TEXT NOT NULL DEFAULT 'ISSUED', created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS fraud_flags (
 flag_id INTEGER PRIMARY KEY, transaction_id INTEGER NOT NULL REFERENCES bank_transactions(transaction_id), reason TEXT NOT NULL,
 status TEXT NOT NULL DEFAULT 'OPEN', created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS audit_logs (
 audit_id INTEGER PRIMARY KEY, actor_user_id INTEGER, event TEXT NOT NULL, details TEXT NOT NULL, created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS loan_payments (
 payment_id INTEGER PRIMARY KEY, loan_id INTEGER NOT NULL REFERENCES loans(loan_id),
 payer_user_id INTEGER NOT NULL REFERENCES users(user_id), amount INTEGER NOT NULL,
 principal_component INTEGER NOT NULL, interest_component INTEGER NOT NULL,
 paid_at TEXT NOT NULL, credit_score_before INTEGER NOT NULL, credit_score_after INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_ledger_account ON ledger_entries(account_id);
CREATE INDEX IF NOT EXISTS idx_requests_status ON approval_requests(status, request_type);
CREATE INDEX IF NOT EXISTS idx_audit_created ON audit_logs(created_at);
)SQL");

    // Older database files may not contain fields added later in the project.
    const auto addColumn = [this](const std::string& table,
                                  const std::string& column,
                                  const std::string& definition) {
        const auto exists = scalarInt(
            "SELECT COUNT(*) FROM pragma_table_info(" + quote(table) + ") "
            "WHERE name=" + quote(column) + ";");
        if (exists == 0) {
            exec("ALTER TABLE " + table + " ADD COLUMN " + column + " " +
                 definition + ";");
        }
    };

    addColumn("users", "dob", "TEXT DEFAULT ''");
    addColumn("users", "address", "TEXT DEFAULT ''");
    addColumn("approval_requests", "requester_dob", "TEXT DEFAULT ''");
    addColumn("approval_requests", "requester_address", "TEXT DEFAULT ''");
}

} // namespace jdn

