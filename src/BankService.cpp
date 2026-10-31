/**
 * @file BankService.cpp
 * @brief Authorization, banking workflows, and ledger posting.
 *
 * This is the application's business layer. The UI asks for an operation;
 * this class validates it, applies the banking rule, and writes the result to
 * SQLite. Keeping this work here prevents the terminal menus from becoming a
 * second copy of the banking rules.
 */
#include "BankService.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace jdn {
namespace {

/** @brief Safely read one text value from a database row. */
std::string field(const Row& row, const std::string& name,
                  const std::string& fallback = "") {
    const auto found = row.find(name);
    return found == row.end() ? fallback : found->second;
}

/** @brief Safely convert one row value to an integer. */
std::int64_t number(const Row& row, const std::string& name,
                    std::int64_t fallback = 0) {
    try {
        return std::stoll(field(row, name));
    } catch (...) {
        return fallback;
    }
}

/** @brief Make the visible part of a transaction reference. */
std::string transactionReferencePrefix() {
    return "JDN" + nowUtc().substr(0, 10) + "-";
}

/** @brief Test whether an account belongs to one of the bank's system accounts. */
bool isSystemAccount(const Database& database, std::int64_t accountId) {
    return database.scalarInt(
               "SELECT COUNT(*) FROM accounts WHERE account_id=" +
               std::to_string(accountId) + " AND user_id IS NULL;") > 0;
}

/** @brief Quote an optional actor id for an INSERT statement. */
std::string actorSql(const std::optional<Session>& actor) {
    return actor ? std::to_string(actor->userId) : "NULL";
}

} // namespace

BankService::BankService(Database& database, Security& security)
    : db_(database), security_(security) {}

void BankService::initialise() {
    db_.migrate();

    // Standing instructions belonged to an older version of the product.
    db_.exec("DROP TABLE IF EXISTS standing_instructions;");

    // NEFT requests are kept until a staff member reviews them.
    db_.exec(
        "CREATE TABLE IF NOT EXISTS neft_queue ("
        "queue_id INTEGER PRIMARY KEY, from_account_id INTEGER NOT NULL, "
        "to_account_id INTEGER NOT NULL, amount INTEGER NOT NULL, "
        "memo TEXT NOT NULL, initiated_by INTEGER, "
        "status TEXT NOT NULL DEFAULT 'PENDING', created_at TEXT NOT NULL, "
        "processed_at TEXT);");

    // System accounts keep bank-owned money inside the same double-entry ledger
    // as customer money. The labels are documentation for people reading data.
    const std::vector<std::string> systemAccounts = {
        "000000000001", // Opening deposits
        "000000000002", // Interest expense
        "000000000003", // Bank fees
        "000000000004"  // Loan fund
    };

    for (const auto& accountNumber : systemAccounts) {
        if (accountByNumber(accountNumber)) {
            continue;
        }

        const auto timestamp = nowUtc();
        db_.exec(
            "INSERT INTO accounts(account_number,user_id,account_type,status,"
            "frozen,min_balance,interest_bps,last_interest_at,created_at) VALUES(" +
            Database::quote(accountNumber) + ",NULL,'SYSTEM','ACTIVE',0,0,0," +
            Database::quote(timestamp) + "," + Database::quote(timestamp) + ");");
    }

    // Repair old databases where closing the account did not close the login.
    db_.exec(
        "UPDATE users SET status='CLOSED' WHERE user_id IN ("
        "SELECT a.user_id FROM accounts a WHERE a.status='CLOSED' "
        "AND a.user_id IS NOT NULL) AND NOT EXISTS ("
        "SELECT 1 FROM accounts active WHERE active.user_id=users.user_id "
        "AND active.status='ACTIVE');");

    const auto loanFund = systemAccount("000000000004");
    if (db_.scalarInt("SELECT COUNT(*) FROM ledger_entries WHERE account_id=" +
                     std::to_string(loanFund) + ";") == 0) {
        postTransfer(systemAccount("000000000001"), loanFund, 1000000000,
                     "BANK_CAPITAL", "JDN Bank opening capital", std::nullopt);
    }

    if (db_.scalarInt("SELECT COUNT(*) FROM users;") == 0) {
        seedDemoData();
    }
}

void BankService::seedDemoData() {
    // A clean local installation receives clearly labelled demo accounts.
    // This makes every dashboard useful immediately for classroom testing.
    const auto created = nowUtc();

    // This lambda is a local function object. It captures db_ and security_
    // from the surrounding BankService object through [this].
    const auto addPerson = [this, &created](
                               const std::string& login,
                               const std::string& role,
                               const std::string& name,
                               const std::string& email,
                               const std::string& phone,
                               const std::string& pan,
                               const std::string& aadhaar,
                               const std::string& password) {
        const auto salt = security_.randomHex(16);

        db_.exec(
            "INSERT INTO users(login,role,name,email,phone,password_hash,"
            "password_salt,created_at) VALUES(" + Database::quote(login) + "," +
            Database::quote(role) + "," + Database::quote(name) + "," +
            Database::quote(email) + "," + Database::quote(phone) + "," +
            Database::quote(security_.hashPassword(password, salt)) + "," +
            Database::quote(salt) + "," + Database::quote(created) + ");");

        const auto userId = db_.lastInsertId();
        db_.exec(
            "INSERT INTO customer_profiles(user_id,aadhaar_enc,pan_enc) VALUES(" +
            std::to_string(userId) + "," + Database::quote(aadhaar) + "," +
            Database::quote(pan) + ");");

        const bool isSavingsAccount = role == "USER";
        db_.exec(
            "INSERT INTO accounts(account_number,user_id,account_type,"
            "min_balance,interest_bps,last_interest_at,created_at) VALUES(" +
            Database::quote(makeAccountNumber(userId, role)) + "," +
            std::to_string(userId) + "," +
            Database::quote(isSavingsAccount ? "SAVINGS" : "CURRENT") + "," +
            std::to_string(isSavingsAccount ? 100000 : 0) + "," +
            std::to_string(isSavingsAccount ? 350 : 0) + "," +
            Database::quote(created) + "," + Database::quote(created) + ");");

        return userId;
    };

    const auto authorityId = addPerson("admin", "AUTHORITY", "Aarav Sharma",
                                       "aarav@jdn.bank", "9876500001",
                                       "ABCDE1234F", "123412341234", "admin123");
    const auto staffId = addPerson("staff", "STAFF", "Neha Verma",
                                   "neha@jdn.bank", "9876500002", "BCDEF2345G",
                                   "234523452345", "staff123");
    const auto firstUserId = addPerson("user1", "USER", "Riya Patel",
                                       "riya@example.com", "9876500003",
                                       "CDEFG3456H", "345634563456", "user123");
    const auto secondUserId = addPerson("user2", "USER", "Kabir Singh",
                                        "kabir@example.com", "9876500004",
                                        "DEFGH4567J", "456745674567", "user123");

    db_.exec(
        "UPDATE users SET dob=CASE role "
        "WHEN 'AUTHORITY' THEN '1980-01-01' "
        "WHEN 'STAFF' THEN '1988-05-14' "
        "WHEN 'USER' THEN '1995-09-21' END, "
        "address=CASE role "
        "WHEN 'AUTHORITY' THEN 'JDN Bank Head Office' "
        "WHEN 'STAFF' THEN 'JDN Bank Staff Quarters' "
        "ELSE 'JDN Bank Customer Address' END;");

    const auto staffAccount = accountsForUser(staffId).front();
    const auto firstUserAccount = accountsForUser(firstUserId).front();
    const auto secondUserAccount = accountsForUser(secondUserId).front();
    const auto openingDeposits = systemAccount("000000000001");

    postTransfer(openingDeposits, number(staffAccount, "account_id"), 1000000,
                 "OPENING_DEPOSIT", "Demo account opening deposit", std::nullopt);
    postTransfer(openingDeposits, number(firstUserAccount, "account_id"), 100000,
                 "OPENING_DEPOSIT", "Demo account opening deposit", std::nullopt);
    postTransfer(openingDeposits, number(secondUserAccount, "account_id"), 100000,
                 "OPENING_DEPOSIT", "Demo account opening deposit", std::nullopt);

    const Session firstUser{firstUserId, "user1", "Riya Patel", Role::User};
    const Session secondUser{secondUserId, "user2", "Kabir Singh", Role::User};
    const Session staff{staffId, "staff", "Neha Verma", Role::Staff};

    requestChequeBook(secondUser, field(secondUserAccount, "account_number"), 10,
                      "14 Lake Road, Pune");
    const auto chequeRequest = db_.scalarInt(
        "SELECT MAX(request_id) FROM approval_requests "
        "WHERE request_type='CHEQUE_BOOK' AND subject_user_id=" +
        std::to_string(secondUserId) + ";");
    if (chequeRequest > 0) {
        staffReview(staff, chequeRequest, true);
    }

    requestLoan(firstUser, 300000, "Professional course fee", 6);
    audit(Session{authorityId, "admin", "Aarav Sharma", Role::Authority},
          "DEMO_DATA_READY",
          "Demo accounts and representative banking activity created.");
}

void BankService::audit(const std::optional<Session>& actor,
                        const std::string& event,
                        const std::string& details) const {
    db_.exec(
        "INSERT INTO audit_logs(actor_user_id,event,details,created_at) VALUES(" +
        actorSql(actor) + "," + Database::quote(event) + "," +
        Database::quote(details) + "," + Database::quote(nowUtc()) + ");");
}

std::optional<Row> BankService::userByLogin(const std::string& login) const {
    const auto rows = db_.query("SELECT * FROM users WHERE login=" +
                                Database::quote(lower(login)) + ";");
    if (rows.empty()) {
        return std::nullopt;
    }
    return rows.front();
}

std::optional<Row> BankService::accountByNumber(const std::string& value) const {
    const auto rows = db_.query("SELECT * FROM accounts WHERE account_number=" +
                                Database::quote(value) + ";");
    if (rows.empty()) {
        return std::nullopt;
    }
    return rows.front();
}

std::optional<Session> BankService::login(Role role, const std::string& login,
                                          const std::string& password,
                                          std::string& message) {
    const auto record = userByLogin(login);
    if (!record || field(*record, "role") != roleName(role)) {
        message = "No matching active login was found.";
        audit(std::nullopt, "LOGIN_FAILED", "Unknown login: " + login);
        return std::nullopt;
    }

    if (field(*record, "status") != "ACTIVE") {
        message = "This account is not active.";
        return std::nullopt;
    }

    const auto lockedUntil = field(*record, "locked_until");
    if (!lockedUntil.empty() &&
        db_.scalarInt(
            "SELECT CASE WHEN datetime('now') < datetime(" +
            Database::quote(lockedUntil) + ") THEN 1 ELSE 0 END;") == 1) {
        message = "Account temporarily locked. Try again after " + lockedUntil +
                  " UTC.";
        return std::nullopt;
    }

    const auto passwordHash = security_.hashPassword(
        password, field(*record, "password_salt"));
    if (passwordHash != field(*record, "password_hash")) {
        const auto failures = number(*record, "failed_attempts") + 1;
        const std::string lockClause =
            failures >= 5 ? ",locked_until=datetime('now','+15 minutes')" : "";

        db_.exec(
            "UPDATE users SET failed_attempts=" + std::to_string(failures) +
            lockClause + " WHERE user_id=" + field(*record, "user_id") + ";");
        audit(std::nullopt, "LOGIN_FAILED",
              "Login " + login + " failure " + std::to_string(failures));

        message = failures >= 5
                      ? "Too many attempts: account locked for 15 minutes."
                      : "Incorrect password.";
        return std::nullopt;
    }

    db_.exec("UPDATE users SET failed_attempts=0,locked_until=NULL WHERE user_id=" +
             field(*record, "user_id") + ";");

    const Session session{number(*record, "user_id"), field(*record, "login"),
                          field(*record, "name"), role};
    audit(session, "LOGIN_SUCCESS", "Authenticated successfully.");
    message = "Welcome back, " + session.name + ".";
    return session;
}

std::string BankService::registerAuthority(
    const std::string& name, const std::string& login,
    const std::string& password, const std::string& email,
    const std::string& phone, const std::string& pan,
    const std::string& aadhaar, const std::string& secret) {
    if (secret != security_.config().authorityRegistrationKey) {
        return "Authority registration key was not accepted.";
    }
    if (name.empty() || login.empty() || password.size() < 8) {
        return "Name and login are required; password must be at least 8 characters.";
    }
    if (userByLogin(login)) {
        return "That login is already in use.";
    }
    if (email.empty() || phone.empty() || pan.empty() || aadhaar.empty()) {
        return "Email, mobile, PAN, and Aadhaar are required for Authority registration.";
    }

    const auto salt = security_.randomHex(16);
    const auto timestamp = nowUtc();
    db_.exec(
        "INSERT INTO users(login,role,name,email,phone,password_hash,password_salt,"
        "created_at) VALUES(" + Database::quote(lower(login)) + ",'AUTHORITY'," +
        Database::quote(name) + "," + Database::quote(email) + "," +
        Database::quote(phone) + "," +
        Database::quote(security_.hashPassword(password, salt)) + "," +
        Database::quote(salt) + "," + Database::quote(timestamp) + ");");

    const auto userId = db_.lastInsertId();
    db_.exec(
        "INSERT INTO customer_profiles(user_id,aadhaar_enc,pan_enc) VALUES(" +
        std::to_string(userId) + "," + Database::quote(aadhaar) + "," +
        Database::quote(pan) + ");");
    db_.exec(
        "INSERT INTO accounts(account_number,user_id,account_type,min_balance,"
        "interest_bps,last_interest_at,created_at) VALUES(" +
        Database::quote(makeAccountNumber(userId, "AUTHORITY")) + "," +
        std::to_string(userId) + ",'CURRENT',0,0," +
        Database::quote(timestamp) + "," + Database::quote(timestamp) + ");");

    audit(std::nullopt, "AUTHORITY_REGISTERED",
          "Authority login " + lower(login) + " created.");
    return "Authority account created. You can now sign in.";
}

std::string BankService::requestStaffRegistration(
    const std::string& name, const std::string& login,
    const std::string& password, const std::string& email,
    const std::string& phone, const std::string& pan,
    const std::string& aadhaar) {
    if (name.empty() || login.empty() || password.size() < 8 || email.empty() ||
        phone.empty() || pan.empty() || aadhaar.empty()) {
        return "Complete all details; password must be at least 8 characters.";
    }

    const auto loginAlreadyRequested = db_.scalarInt(
        "SELECT COUNT(*) FROM approval_requests WHERE requester_login=" +
        Database::quote(lower(login)) +
        " AND status IN ('PENDING','FORWARDED');");
    if (userByLogin(login) || loginAlreadyRequested > 0) {
        return "That login already exists or is awaiting review.";
    }

    const auto salt = security_.randomHex(16);
    const auto timestamp = nowUtc();
    db_.exec(
        "INSERT INTO approval_requests(request_type,requester_name,requester_login,"
        "requester_password_hash,requester_password_salt,requester_email,"
        "requester_phone,pan_enc,aadhaar_enc,status,created_at,updated_at) VALUES("
        "'STAFF_REGISTRATION'," + Database::quote(name) + "," +
        Database::quote(lower(login)) + "," +
        Database::quote(security_.hashPassword(password, salt)) + "," +
        Database::quote(salt) + "," + Database::quote(email) + "," +
        Database::quote(phone) + "," + Database::quote(pan) + "," +
        Database::quote(aadhaar) + ",'PENDING'," +
        Database::quote(timestamp) + "," + Database::quote(timestamp) + ");");

    audit(std::nullopt, "STAFF_REGISTRATION_REQUEST",
          "Staff registration requested: " + lower(login));
    return "Staff registration submitted to Authority.";
}

std::string BankService::requestUserRegistration(
    const std::string& name, const std::string& login,
    const std::string& password, const std::string& email,
    const std::string& phone, const std::string& aadhaar,
    const std::string& pan, Money opening,
    std::optional<std::int64_t> staffId) {
    if (name.empty() || login.empty() || password.size() < 8 || aadhaar.empty() ||
        pan.empty() || opening < 0) {
        return "Complete all required fields; password needs 8 characters.";
    }
    if (userByLogin(login)) {
        return "That login is already in use.";
    }

    if (!staffId) {
        staffId = db_.scalarInt(
            "SELECT user_id FROM users WHERE role='STAFF' AND status='ACTIVE' "
            "ORDER BY user_id LIMIT 1;",
            0);
        if (*staffId == 0) {
            return "No active staff member is available yet.";
        }
    } else {
        staffId = db_.scalarInt(
            "SELECT user_id FROM (SELECT user_id,ROW_NUMBER() OVER "
            "(ORDER BY user_id) AS staff_number FROM users "
            "WHERE role='STAFF' AND status='ACTIVE') WHERE staff_number=" +
                std::to_string(*staffId) + ";",
            0);
    }
    if (*staffId == 0) {
        return "Selected staff ID is unavailable.";
    }

    const auto salt = security_.randomHex(16);
    const auto timestamp = nowUtc();
    db_.exec(
        "INSERT INTO approval_requests(request_type,requester_name,requester_login,"
        "requester_password_hash,requester_password_salt,requester_email,"
        "requester_phone,aadhaar_enc,pan_enc,opening_balance,assigned_staff_id,"
        "status,created_at,updated_at) VALUES('USER_REGISTRATION'," +
        Database::quote(name) + "," + Database::quote(lower(login)) + "," +
        Database::quote(security_.hashPassword(password, salt)) + "," +
        Database::quote(salt) + "," + Database::quote(email) + "," +
        Database::quote(phone) + "," + Database::quote(aadhaar) + "," +
        Database::quote(pan) + "," + std::to_string(opening) + "," +
        std::to_string(*staffId) + ",'PENDING'," + Database::quote(timestamp) +
        "," + Database::quote(timestamp) + ");");

    audit(std::nullopt, "USER_REGISTRATION_REQUEST",
          "User registration requested: " + lower(login));
    return "Registration submitted to Staff for review.";
}

std::string BankService::registerAuthorityFull(
    const std::string& name, const std::string& mobile,
    const std::string& dob, const std::string& email,
    const std::string& address, const std::string& pan,
    const std::string& aadhaar, const std::string& login,
    const std::string& password, const std::string& secret) {
    if (secret != security_.config().authorityRegistrationKey) {
        return "Authority registration key was not accepted.";
    }
    if (name.empty() || mobile.empty() || dob.empty() || email.empty() ||
        address.empty() || pan.empty() || aadhaar.empty() || login.empty() ||
        password.size() < 8) {
        return "Please complete every field; password must be at least 8 characters.";
    }
    if (userByLogin(login)) {
        return "That user ID is already in use.";
    }

    const auto salt = security_.randomHex(16);
    const auto timestamp = nowUtc();
    db_.exec(
        "INSERT INTO users(login,role,name,email,phone,dob,address,password_hash,"
        "password_salt,created_at) VALUES(" + Database::quote(lower(login)) +
        ",'AUTHORITY'," + Database::quote(name) + "," +
        Database::quote(email) + "," + Database::quote(mobile) + "," +
        Database::quote(dob) + "," + Database::quote(address) + "," +
        Database::quote(security_.hashPassword(password, salt)) + "," +
        Database::quote(salt) + "," + Database::quote(timestamp) + ");");

    const auto userId = db_.lastInsertId();
    db_.exec(
        "INSERT INTO customer_profiles(user_id,aadhaar_enc,pan_enc) VALUES(" +
        std::to_string(userId) + "," + Database::quote(aadhaar) + "," +
        Database::quote(pan) + ");");
    db_.exec(
        "INSERT INTO accounts(account_number,user_id,account_type,min_balance,"
        "interest_bps,last_interest_at,created_at) VALUES(" +
        Database::quote(makeAccountNumber(userId, "AUTHORITY")) + "," +
        std::to_string(userId) + ",'CURRENT',0,0," +
        Database::quote(timestamp) + "," + Database::quote(timestamp) + ");");

    audit(std::nullopt, "AUTHORITY_REGISTERED",
          "Authority account opened for " + lower(login));
    return "Authority account created. You can now log in.";
}

std::string BankService::requestStaffRegistrationFull(
    const std::string& name, const std::string& mobile,
    const std::string& dob, const std::string& email,
    const std::string& address, const std::string& pan,
    const std::string& aadhaar, const std::string& login,
    const std::string& password) {
    if (name.empty() || mobile.empty() || dob.empty() || email.empty() ||
        address.empty() || pan.empty() || aadhaar.empty() || login.empty() ||
        password.size() < 8) {
        return "Please complete every field; password must be at least 8 characters.";
    }

    const auto loginAlreadyRequested = db_.scalarInt(
        "SELECT COUNT(*) FROM approval_requests WHERE requester_login=" +
        Database::quote(lower(login)) +
        " AND status IN ('PENDING','FORWARDED');");
    if (userByLogin(login) || loginAlreadyRequested > 0) {
        return "That user ID already exists or is awaiting approval.";
    }

    const auto salt = security_.randomHex(16);
    const auto timestamp = nowUtc();
    db_.exec(
        "INSERT INTO approval_requests(request_type,requester_name,requester_login,"
        "requester_password_hash,requester_password_salt,requester_email,"
        "requester_phone,requester_dob,requester_address,pan_enc,aadhaar_enc,"
        "status,created_at,updated_at) VALUES('STAFF_REGISTRATION'," +
        Database::quote(name) + "," + Database::quote(lower(login)) + "," +
        Database::quote(security_.hashPassword(password, salt)) + "," +
        Database::quote(salt) + "," + Database::quote(email) + "," +
        Database::quote(mobile) + "," + Database::quote(dob) + "," +
        Database::quote(address) + "," + Database::quote(pan) + "," +
        Database::quote(aadhaar) + ",'PENDING'," +
        Database::quote(timestamp) + "," + Database::quote(timestamp) + ");");

    audit(std::nullopt, "STAFF_REGISTRATION_REQUEST",
          "Staff registration requested: " + lower(login));
    return "Staff registration sent for Authority approval.";
}

std::string BankService::requestUserRegistrationFull(
    const std::string& name, const std::string& mobile,
    const std::string& dob, const std::string& email,
    const std::string& address, const std::string& pan,
    const std::string& aadhaar, const std::string& login,
    const std::string& password) {
    if (name.empty() || mobile.empty() || dob.empty() || email.empty() ||
        address.empty() || pan.empty() || aadhaar.empty() || login.empty() ||
        password.size() < 8) {
        return "Please complete every field; password must be at least 8 characters.";
    }
    if (userByLogin(login)) {
        return "That user ID is already in use.";
    }

    const auto staff = db_.scalarInt(
        "SELECT user_id FROM users WHERE role='STAFF' AND status='ACTIVE' "
        "ORDER BY RANDOM() LIMIT 1;",
        0);
    if (staff == 0) {
        return "No active Staff member is available for this request.";
    }

    const auto salt = security_.randomHex(16);
    const auto timestamp = nowUtc();
    db_.exec(
        "INSERT INTO approval_requests(request_type,requester_name,requester_login,"
        "requester_password_hash,requester_password_salt,requester_email,"
        "requester_phone,requester_dob,requester_address,aadhaar_enc,pan_enc,"
        "assigned_staff_id,status,created_at,updated_at) VALUES('USER_REGISTRATION'," +
        Database::quote(name) + "," + Database::quote(lower(login)) + "," +
        Database::quote(security_.hashPassword(password, salt)) + "," +
        Database::quote(salt) + "," + Database::quote(email) + "," +
        Database::quote(mobile) + "," + Database::quote(dob) + "," +
        Database::quote(address) + "," + Database::quote(aadhaar) + "," +
        Database::quote(pan) + "," + std::to_string(staff) + ",'PENDING'," +
        Database::quote(timestamp) + "," + Database::quote(timestamp) + ");");

    audit(std::nullopt, "USER_REGISTRATION_REQUEST",
          "User registration requested: " + lower(login));
    return "Registration sent for Staff approval.";
}

Rows BankService::staffDirectory() const {
    return db_.query(
        "SELECT ROW_NUMBER() OVER (ORDER BY user_id) AS staff_id,user_id,login,"
        "name,email,created_at FROM users WHERE role='STAFF' AND status='ACTIVE' "
        "ORDER BY user_id;");
}

Rows BankService::requestsFor(const Session& session) const {
    const std::string columns =
        "SELECT r.request_id,r.request_type,"
        "COALESCE(NULLIF(r.requester_name,''),u.name,'-') AS requester_name,"
        "COALESCE(NULLIF(r.requester_login,''),u.login,'-') AS requester_login,"
        "CASE WHEN r.amount>0 THEN r.amount ELSE r.opening_balance END AS amount,"
        "r.metadata,r.status,r.created_at FROM approval_requests r "
        "LEFT JOIN users u ON u.user_id=r.subject_user_id ";

    if (session.role == Role::Staff) {
        return db_.query(
            columns +
            "WHERE ((r.request_type='USER_REGISTRATION' AND r.assigned_staff_id=" +
            std::to_string(session.userId) + ") OR r.request_type IN "
            "('LOAN','CHEQUE_BOOK','ADD_MONEY','NEFT_TRANSFER')) "
            "AND r.status IN ('PENDING','FORWARDED') ORDER BY r.request_id;");
    }
    if (session.role == Role::Authority) {
        return db_.query(columns +
                         "WHERE r.status IN ('PENDING','FORWARDED') "
                         "ORDER BY r.request_id;");
    }
    return {};
}

std::string BankService::staffReview(const Session& staff,
                                     std::int64_t requestId, bool approve) {
    if (staff.role != Role::Staff) {
        return "Not authorized.";
    }

    const auto rows = db_.query(
        "SELECT * FROM approval_requests WHERE request_id=" +
        std::to_string(requestId) +
        " AND status IN ('PENDING','FORWARDED') AND ((request_type="
        "'USER_REGISTRATION' AND assigned_staff_id=" +
        std::to_string(staff.userId) + ") OR request_type IN "
        "('LOAN','CHEQUE_BOOK','ADD_MONEY','NEFT_TRANSFER'));" );
    if (rows.empty()) {
        return "No pending Staff request found.";
    }

    const auto& request = rows.front();
    const auto type = field(request, "request_type");

    if (approve && type == "USER_REGISTRATION") {
        createApprovedUser(request, staff.userId);
    }

    if (approve && type == "CHEQUE_BOOK") {
        const auto metadata = field(request, "metadata");
        const auto separator = metadata.find('|');
        const auto accountNumber = separator == std::string::npos
                                        ? metadata
                                        : metadata.substr(0, separator);
        const auto account = accountByNumber(accountNumber);
        if (!account) {
            return "The requested account no longer exists.";
        }

        int leaves = 10;
        if (separator != std::string::npos) {
            try {
                leaves = std::stoi(metadata.substr(separator + 1));
            } catch (...) {
                // Keep the safe default of ten leaves for malformed metadata.
            }
        }
        for (int leaf = 0; leaf < leaves; ++leaf) {
            const auto chequeNumber =
                "CHQ" + std::to_string(number(*account, "account_id")) +
                security_.randomHex(4);
            db_.exec(
                "INSERT INTO cheques(account_id,cheque_number,created_at) VALUES(" +
                field(*account, "account_id") + "," +
                Database::quote(chequeNumber) + "," + Database::quote(nowUtc()) + ");");
        }
    }

    if (approve && type == "LOAN") {
        db_.exec("UPDATE loans SET status='STAFF_REVIEWED' WHERE loan_id=" +
                 field(request, "metadata") + ";");
    }

    if (approve && type == "NEFT_TRANSFER") {
        const auto metadata = field(request, "metadata");
        const auto firstSeparator = metadata.find('|');
        const auto secondSeparator = metadata.find('|', firstSeparator + 1);
        if (firstSeparator == std::string::npos ||
            secondSeparator == std::string::npos) {
            return "Invalid NEFT request.";
        }

        const auto fromAccount =
            std::stoll(metadata.substr(0, firstSeparator));
        const auto toAccount = std::stoll(
            metadata.substr(firstSeparator + 1,
                            secondSeparator - firstSeparator - 1));
        const auto memo = metadata.substr(secondSeparator + 1);
        postTransfer(fromAccount, toAccount, number(request, "amount"), "NEFT",
                     memo, staff);
        postTransfer(fromAccount, systemAccount("000000000003"), 1000,
                     "NEFT_FEE", "NEFT service fee", staff);
    }

    // ADD_MONEY has its own posting logic because approval creates a deposit.
    if (type == "ADD_MONEY") {
        return approveAddMoney(staff, requestId, approve);
    }

    const auto status = approve && type == "LOAN"
                            ? "FORWARDED"
                            : approve ? "APPROVED" : "REJECTED";
    db_.exec(
        "UPDATE approval_requests SET status=" + Database::quote(status) +
        ",staff_reviewer_id=" + std::to_string(staff.userId) +
        ",updated_at=" + Database::quote(nowUtc()) +
        " WHERE request_id=" + std::to_string(requestId) + ";");
    audit(staff, std::string("STAFF_") + status,
          type + " request #" + std::to_string(requestId));

    if (type == "LOAN" && approve) {
        return "Loan reviewed and forwarded to Authority.";
    }
    return approve ? "Request approved by Staff." : "Request rejected by Staff.";
}

void BankService::createApprovedUser(const Row& request,
                                     std::int64_t authorityId) {
    const auto timestamp = nowUtc();
    db_.exec(
        "INSERT INTO users(login,role,name,email,phone,dob,address,password_hash,"
        "password_salt,created_at) VALUES(" +
        Database::quote(field(request, "requester_login")) + ",'USER'," +
        Database::quote(field(request, "requester_name")) + "," +
        Database::quote(field(request, "requester_email")) + "," +
        Database::quote(field(request, "requester_phone")) + "," +
        Database::quote(field(request, "requester_dob")) + "," +
        Database::quote(field(request, "requester_address")) + "," +
        Database::quote(field(request, "requester_password_hash")) + "," +
        Database::quote(field(request, "requester_password_salt")) + "," +
        Database::quote(timestamp) + ");");

    const auto userId = db_.lastInsertId();
    db_.exec(
        "INSERT INTO customer_profiles(user_id,aadhaar_enc,pan_enc) VALUES(" +
        std::to_string(userId) + "," +
        Database::quote(field(request, "aadhaar_enc")) + "," +
        Database::quote(field(request, "pan_enc")) + ");");

    const auto accountNumber = makeAccountNumber(userId, "USER");
    db_.exec(
        "INSERT INTO accounts(account_number,user_id,last_interest_at,created_at) "
        "VALUES(" + Database::quote(accountNumber) + "," +
        std::to_string(userId) + "," + Database::quote(timestamp) + "," +
        Database::quote(timestamp) + ");");

    const auto accountId = db_.lastInsertId();
    const auto openingBalance = number(request, "opening_balance");
    if (openingBalance > 0) {
        postTransfer(systemAccount("000000000001"), accountId, openingBalance,
                     "OPENING_DEPOSIT", "Approved opening deposit", std::nullopt);
    }

    db_.exec("UPDATE approval_requests SET subject_user_id=" +
             std::to_string(userId) + " WHERE request_id=" +
             field(request, "request_id") + ";");
    audit(Session{authorityId, "", "", Role::Authority}, "USER_ACCOUNT_CREATED",
          accountNumber);
}

void BankService::createApprovedStaff(const Row& request,
                                      std::int64_t authorityId) {
    const auto timestamp = nowUtc();
    db_.exec(
        "INSERT INTO users(login,role,name,email,phone,dob,address,password_hash,"
        "password_salt,created_at) VALUES(" +
        Database::quote(field(request, "requester_login")) + ",'STAFF'," +
        Database::quote(field(request, "requester_name")) + "," +
        Database::quote(field(request, "requester_email")) + "," +
        Database::quote(field(request, "requester_phone")) + "," +
        Database::quote(field(request, "requester_dob")) + "," +
        Database::quote(field(request, "requester_address")) + "," +
        Database::quote(field(request, "requester_password_hash")) + "," +
        Database::quote(field(request, "requester_password_salt")) + "," +
        Database::quote(timestamp) + ");");

    const auto userId = db_.lastInsertId();
    db_.exec(
        "INSERT INTO customer_profiles(user_id,aadhaar_enc,pan_enc) VALUES(" +
        std::to_string(userId) + "," +
        Database::quote(field(request, "aadhaar_enc")) + "," +
        Database::quote(field(request, "pan_enc")) + ");");
    db_.exec(
        "INSERT INTO accounts(account_number,user_id,account_type,min_balance,"
        "interest_bps,last_interest_at,created_at) VALUES(" +
        Database::quote(makeAccountNumber(userId, "STAFF")) + "," +
        std::to_string(userId) + ",'CURRENT',0,0," +
        Database::quote(timestamp) + "," + Database::quote(timestamp) + ");");

    audit(Session{authorityId, "", "", Role::Authority},
          "STAFF_ACCOUNT_CREATED", "Staff account created.");
}

std::string BankService::authorityReview(const Session& authority,
                                         std::int64_t requestId, bool approve) {
    if (authority.role != Role::Authority) {
        return "Not authorized.";
    }

    const auto rows = db_.query(
        "SELECT * FROM approval_requests WHERE request_id=" +
        std::to_string(requestId) +
        " AND status IN ('PENDING','FORWARDED');");
    if (rows.empty()) {
        return "No active request found.";
    }

    const auto& request = rows.front();
    const auto type = field(request, "request_type");

    if (approve) {
        if (type == "USER_REGISTRATION") {
            createApprovedUser(request, authority.userId);
        } else if (type == "STAFF_REGISTRATION") {
            createApprovedStaff(request, authority.userId);
        } else if (type == "LOAN") {
            db_.exec("UPDATE loans SET status='APPROVED' WHERE loan_id=" +
                     field(request, "metadata") + ";");
        } else if (type == "CHEQUE_BOOK") {
            const auto metadata = field(request, "metadata");
            const auto separator = metadata.find('|');
            const auto accountNumber = separator == std::string::npos
                                            ? metadata
                                            : metadata.substr(0, separator);
            const auto account = accountByNumber(accountNumber);
            if (!account) {
                throw std::runtime_error(
                    "The requested account no longer exists.");
            }

            if (db_.scalarInt(
                    "SELECT COUNT(*) FROM cheques WHERE account_id=" +
                    field(*account, "account_id") + ";") == 0) {
                int leaves = 10;
                if (separator != std::string::npos) {
                    try {
                        leaves = std::stoi(metadata.substr(separator + 1));
                    } catch (...) {
                        // Keep the default of ten leaves.
                    }
                }
                for (int leaf = 0; leaf < leaves; ++leaf) {
                    const auto chequeNumber =
                        "CHQ" + std::to_string(number(*account, "account_id")) +
                        security_.randomHex(4);
                    db_.exec(
                        "INSERT INTO cheques(account_id,cheque_number,created_at) "
                        "VALUES(" + field(*account, "account_id") + "," +
                        Database::quote(chequeNumber) + "," +
                        Database::quote(nowUtc()) + ");");
                }
            }
        } else if (type == "RTGS_TRANSFER") {
            const auto metadata = field(request, "metadata");
            const auto firstSeparator = metadata.find('|');
            const auto secondSeparator = metadata.find('|', firstSeparator + 1);
            if (firstSeparator == std::string::npos ||
                secondSeparator == std::string::npos) {
                throw std::runtime_error("Invalid RTGS request metadata.");
            }

            const auto fromAccount =
                std::stoll(metadata.substr(0, firstSeparator));
            const auto toAccount = std::stoll(
                metadata.substr(firstSeparator + 1,
                                secondSeparator - firstSeparator - 1));
            postTransfer(fromAccount, toAccount, number(request, "amount"), "RTGS",
                         metadata.substr(secondSeparator + 1), authority);
            postTransfer(fromAccount, systemAccount("000000000003"), 2500,
                         "RTGS_FEE", "RTGS service fee", authority);
        } else if (type == "ACCOUNT_CLOSURE") {
            const auto account = accountByNumber(field(request, "metadata"));
            if (!account) {
                throw std::runtime_error("Closure account no longer exists.");
            }
            if (balance(number(*account, "account_id")) != 0) {
                throw std::runtime_error(
                    "Account must have a zero balance before closure.");
            }

            db_.exec("UPDATE accounts SET status='CLOSED' WHERE account_id=" +
                     field(*account, "account_id") + ";");
            // Closing the account also removes login access after sign-out.
            db_.exec("UPDATE users SET status='CLOSED' WHERE user_id=" +
                     field(request, "subject_user_id") + ";");
        }
    }

    const auto status = approve ? "APPROVED" : "REJECTED";
    db_.exec(
        "UPDATE approval_requests SET status=" + Database::quote(status) +
        ",authority_reviewer_id=" + std::to_string(authority.userId) +
        ",updated_at=" + Database::quote(nowUtc()) +
        " WHERE request_id=" + std::to_string(requestId) + ";");
    audit(authority, std::string("REQUEST_") + status,
          type + " #" + std::to_string(requestId));
    return approve ? "Request approved." : "Request rejected.";
}

std::string BankService::makeAccountNumber(std::int64_t seed,
                                           const std::string& role) const {
    const char prefix = role == "AUTHORITY" ? '1' : role == "STAFF" ? '2' : '3';
    std::ostringstream base;
    base << prefix << std::setw(10) << std::setfill('0') << seed;
    const auto numberWithoutCheckDigit = base.str();

    int sum = 0;
    for (std::size_t index = 0; index < numberWithoutCheckDigit.size(); ++index) {
        int digit = numberWithoutCheckDigit[index] - '0';
        if (index % 2 == 0) {
            digit *= 2;
            if (digit > 9) {
                digit -= 9;
            }
        }
        sum += digit;
    }

    return numberWithoutCheckDigit + std::to_string((10 - sum % 10) % 10);
}

bool BankService::validAccountNumber(const std::string& value) const {
    if (value.size() != 12 ||
        !std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return std::isdigit(character) != 0;
        })) {
        return false;
    }

    int sum = 0;
    for (std::size_t index = 0; index < value.size(); ++index) {
        int digit = value[index] - '0';
        if (index % 2 == 0) {
            digit *= 2;
            if (digit > 9) {
                digit -= 9;
            }
        }
        sum += digit;
    }

    // System accounts intentionally use a zero prefix and are also accepted.
    return sum % 10 == 0 || value.rfind("00000000000", 0) == 0;
}

std::int64_t BankService::systemAccount(const std::string& accountNumber) const {
    const auto account = accountByNumber(accountNumber);
    if (!account) {
        throw std::runtime_error("System account missing.");
    }
    return number(*account, "account_id");
}

Money BankService::balance(std::int64_t accountId) {
    return db_.scalarInt(
        "SELECT COALESCE(SUM(CASE entry_side WHEN 'CREDIT' THEN amount "
        "ELSE -amount END),0) FROM ledger_entries WHERE account_id=" +
        std::to_string(accountId) + ";");
}

Rows BankService::accountsForUser(std::int64_t userId,
                                  bool includeClosed) const {
    const auto statusFilter = includeClosed ? "" : " AND a.status='ACTIVE'";
    return db_.query(
        "SELECT a.account_id,a.account_number,a.account_type,a.status,a.frozen,"
        "a.min_balance,a.interest_bps,COALESCE(SUM(CASE l.entry_side "
        "WHEN 'CREDIT' THEN l.amount ELSE -l.amount END),0) AS balance "
        "FROM accounts a LEFT JOIN ledger_entries l ON a.account_id=l.account_id "
        "WHERE a.user_id=" + std::to_string(userId) + statusFilter +
        " GROUP BY a.account_id ORDER BY a.account_id;");
}

Rows BankService::allAccounts() const {
    return db_.query(
        "SELECT a.account_number,COALESCE(u.login,'SYSTEM') AS owner,a.status,"
        "a.frozen,COALESCE(SUM(CASE l.entry_side WHEN 'CREDIT' THEN l.amount "
        "ELSE -l.amount END),0) AS balance FROM accounts a "
        "LEFT JOIN users u ON u.user_id=a.user_id "
        "LEFT JOIN ledger_entries l ON l.account_id=a.account_id "
        "GROUP BY a.account_id ORDER BY a.account_id;");
}

Rows BankService::informationForAuthority(const Session& authority,
                                           const std::string& role) const {
    if (authority.role != Role::Authority) {
        return {};
    }

    const auto people = db_.query(
        "SELECT u.user_id,u.role,u.login,u.name,u.email,u.phone,u.dob,u.address,"
        "u.credit_score,a.account_number,COALESCE(SUM(CASE l.entry_side "
        "WHEN 'CREDIT' THEN l.amount ELSE -l.amount END),0) AS balance "
        "FROM users u LEFT JOIN accounts a ON a.user_id=u.user_id "
        "LEFT JOIN ledger_entries l ON l.account_id=a.account_id "
        "WHERE u.role=" + Database::quote(role) +
        " AND u.status='ACTIVE' GROUP BY u.user_id,a.account_id "
        "ORDER BY u.user_id;");

    Rows result = people;
    for (auto& person : result) {
        if (field(person, "role") != "USER" &&
            field(person, "role") != "AUTHORITY") {
            continue;
        }

        const auto profile = db_.query(
            "SELECT aadhaar_enc,pan_enc FROM customer_profiles WHERE user_id=" +
            field(person, "user_id") + ";");
        if (!profile.empty()) {
            person["aadhaar"] = field(profile.front(), "aadhaar_enc");
            person["pan"] = field(profile.front(), "pan_enc");
        }

        const auto loans = db_.query(
            "SELECT loan_id,status,outstanding FROM loans WHERE user_id=" +
            field(person, "user_id") + " ORDER BY loan_id;");
        for (const auto& loan : loans) {
            person["loan_details"] +=
                "#" + field(loan, "loan_id") + " " + field(loan, "status") +
                " " + formatMoney(number(loan, "outstanding")) + "; ";
        }
    }
    return result;
}

Rows BankService::informationForStaff(const Session& staff) const {
    if (staff.role != Role::Staff) {
        return {};
    }

    auto rows = informationForAuthority(
        Session{0, "", "", Role::Authority}, "USER");
    for (auto& row : rows) {
        if (row.contains("pan") && !row["pan"].empty()) {
            row["pan"] = "********";
        }
        if (row.contains("aadhaar") && !row["aadhaar"].empty()) {
            row["aadhaar"] = "********";
        }
    }
    return rows;
}

Rows BankService::ownInformation(const Session& user) const {
    const auto rows = db_.query(
        "SELECT u.user_id,u.role,u.login,u.name,u.email,u.phone,u.dob,u.address,"
        "u.credit_score,a.account_number,COALESCE(SUM(CASE l.entry_side "
        "WHEN 'CREDIT' THEN l.amount ELSE -l.amount END),0) AS balance "
        "FROM users u LEFT JOIN accounts a ON a.user_id=u.user_id "
        "LEFT JOIN ledger_entries l ON l.account_id=a.account_id "
        "WHERE u.user_id=" + std::to_string(user.userId) +
        " GROUP BY u.user_id,a.account_id;");

    Rows result = rows;
    if (result.empty()) {
        return result;
    }

    const auto profile = db_.query(
        "SELECT aadhaar_enc,pan_enc FROM customer_profiles WHERE user_id=" +
        std::to_string(user.userId) + ";");
    if (!profile.empty()) {
        result.front()["aadhaar"] = field(profile.front(), "aadhaar_enc");
        result.front()["pan"] = field(profile.front(), "pan_enc");
    }

    const auto loans = db_.query(
        "SELECT loan_id,status,outstanding FROM loans WHERE user_id=" +
        std::to_string(user.userId) + " ORDER BY loan_id;");
    for (const auto& loan : loans) {
        result.front()["loan_details"] +=
            "#" + field(loan, "loan_id") + " " + field(loan, "status") + " " +
            formatMoney(number(loan, "outstanding")) + "; ";
    }
    return result;
}

void BankService::postTransfer(std::int64_t fromAccount,
                               std::int64_t toAccount, Money amount,
                               const std::string& type,
                               const std::string& memo,
                               const std::optional<Session>& actor) {
    if (amount <= 0) {
        throw std::runtime_error("Amount must be greater than zero.");
    }
    if (fromAccount == toAccount) {
        throw std::runtime_error("Source and destination accounts must differ.");
    }
    if (!isSystemAccount(db_, fromAccount) && balance(fromAccount) < amount) {
        throw std::runtime_error("Insufficient available balance.");
    }

    const auto timestamp = nowUtc();
    Transaction transaction(db_);
    const auto reference = transactionReferencePrefix() + security_.randomHex(4);

    db_.exec(
        "INSERT INTO bank_transactions(transaction_ref,transaction_type,status,"
        "initiated_by,memo,created_at,previous_hash,current_hash) VALUES(" +
        Database::quote(reference) + "," + Database::quote(type) + ",'COMPLETED'," +
        actorSql(actor) + "," + Database::quote(memo) + "," +
        Database::quote(timestamp) + ",'','');");
    const auto transactionId = db_.lastInsertId();

    db_.exec(
        "INSERT INTO ledger_entries(transaction_id,account_id,entry_side,amount,"
        "created_at) VALUES(" + std::to_string(transactionId) + "," +
        std::to_string(fromAccount) + ",'DEBIT'," + std::to_string(amount) + "," +
        Database::quote(timestamp) + "),(" + std::to_string(transactionId) + "," +
        std::to_string(toAccount) + ",'CREDIT'," + std::to_string(amount) + "," +
        Database::quote(timestamp) + ");");

    flagFraud(transactionId, toAccount, amount, actor);
    transaction.commit();
    audit(actor, "TRANSACTION_POSTED",
          reference + " " + type + " " + formatMoney(amount));
}

void BankService::flagFraud(std::int64_t transactionId,
                            std::int64_t recipientAccount, Money amount,
                            const std::optional<Session>& actor) {
    std::vector<std::string> reasons;
    if (amount >= 10000000) {
        reasons.emplace_back("Large-value transaction (₹100,000 or more)");
    }
    if (db_.scalarInt(
            "SELECT COUNT(*) FROM bank_transactions WHERE initiated_by=" +
            (actor ? std::to_string(actor->userId) : "-1") +
            " AND created_at >= datetime('now','-10 minutes');") >= 5) {
        reasons.emplace_back("High transfer frequency in 10 minutes");
    }
    if (db_.scalarInt(
            "SELECT COUNT(*) FROM accounts WHERE account_id=" +
            std::to_string(recipientAccount) +
            " AND created_at >= datetime('now','-7 days');") > 0) {
        reasons.emplace_back("Transfer to an account opened within 7 days");
    }

    for (const auto& reason : reasons) {
        db_.exec(
            "INSERT INTO fraud_flags(transaction_id,reason,created_at) VALUES(" +
            std::to_string(transactionId) + "," + Database::quote(reason) + "," +
            Database::quote(nowUtc()) + ");");
    }
}

void BankService::accrueInterest(std::int64_t accountId) {
    const auto rows = db_.query(
        "SELECT * FROM accounts WHERE account_id=" + std::to_string(accountId) +
        " AND account_type='SAVINGS' AND status='ACTIVE';");
    if (rows.empty()) {
        return;
    }

    const auto days = db_.scalarInt(
        "SELECT CAST(julianday('now')-julianday(" +
        Database::quote(field(rows.front(), "last_interest_at")) + ") AS INTEGER);");
    if (days <= 0) {
        return;
    }

    const auto interest =
        (balance(accountId) * number(rows.front(), "interest_bps") * days) /
        (365 * 10000);
    if (interest > 0) {
        postTransfer(systemAccount("000000000002"), accountId, interest,
                     "SAVINGS_INTEREST", "Daily interest for " +
                                             std::to_string(days) + " day(s)",
                     std::nullopt);
    }

    const auto currentBalance = balance(accountId);
    if (currentBalance < number(rows.front(), "min_balance")) {
        postTransfer(accountId, systemAccount("000000000003"), 500,
                     "MINIMUM_BALANCE_FEE", "Minimum balance service charge",
                     std::nullopt);
    }

    db_.exec("UPDATE accounts SET last_interest_at=" +
             Database::quote(nowUtc()) + " WHERE account_id=" +
             std::to_string(accountId) + ";");
}

std::string BankService::transfer(const Session& actor,
                                  const std::string& fromNumber,
                                  const std::string& toNumber, Money amount,
                                  const std::string& rail,
                                  const std::string& memo) {
    if (actor.role != Role::User && actor.role != Role::Staff) {
        return "Only Staff or User accounts can initiate transfers.";
    }

    const auto from = accountByNumber(fromNumber);
    if (!from || number(*from, "user_id") != actor.userId) {
        return "Choose one of your own account numbers.";
    }
    if (!validAccountNumber(toNumber)) {
        return "Recipient account number failed checksum validation.";
    }

    const auto to = accountByNumber(toNumber);
    if (!to || field(*to, "status") != "ACTIVE" || number(*to, "frozen") != 0) {
        return "Recipient account is unavailable.";
    }
    if (field(*from, "status") != "ACTIVE" || number(*from, "frozen") != 0) {
        return "Your account is unavailable or frozen.";
    }

    const auto fromId = number(*from, "account_id");
    const auto toId = number(*to, "account_id");
    accrueInterest(fromId);

    if (rail == "RTGS" && amount < 20000000) {
        return "RTGS requires a minimum of ₹200,000.00.";
    }
    if (rail == "IMPS" && amount > 500000) {
        return "IMPS limit is ₹5,000.00.";
    }

    // NEFT and RTGS are intentionally approval-based.
    if (rail == "NEFT" || rail == "RTGS") {
        Transaction transaction(db_);
        const auto metadata = std::to_string(fromId) + "|" +
                              std::to_string(toId) + "|" + memo;
        db_.exec(
            "INSERT INTO approval_requests(request_type,subject_user_id,amount,"
            "metadata,status,created_at,updated_at) VALUES(" +
            Database::quote(rail + "_TRANSFER") + "," +
            std::to_string(actor.userId) + "," + std::to_string(amount) + "," +
            Database::quote(metadata) + ",'PENDING'," + Database::quote(nowUtc()) +
            "," + Database::quote(nowUtc()) + ");");
        transaction.commit();

        audit(actor, rail + "_APPROVAL_REQUESTED",
              "Requested " + formatMoney(amount) +
                  (rail == "NEFT" ? " for Staff approval"
                                  : "; Authority approval required"));
        return rail == "NEFT" ? "NEFT transfer sent to Staff for approval."
                               : "RTGS transfer submitted to Authority for approval.";
    }

    const Money fee = rail == "RTGS" ? 2500 : 0;
    if (balance(fromId) < amount + fee) {
        return "Insufficient balance including the applicable fee.";
    }

    postTransfer(fromId, toId, amount, rail, memo, actor);
    if (fee > 0) {
        postTransfer(fromId, systemAccount("000000000003"), fee,
                     rail + "_FEE", "RTGS service fee", actor);
    }
    return rail + " transfer completed: " + formatMoney(amount) + ".";
}

std::string BankService::transferByCheque(
    const Session& actor, const std::string& fromNumber,
    const std::string& toNumber, const std::string& chequeNumber,
    Money amount, const std::string& recipientName) {
    if (actor.role != Role::User && actor.role != Role::Staff) {
        return "Only Staff or User accounts can transfer by cheque.";
    }

    const auto from = accountByNumber(fromNumber);
    const auto to = accountByNumber(toNumber);
    if (!from || number(*from, "user_id") != actor.userId) {
        return "Choose one of your own account numbers.";
    }
    if (!to || field(*to, "status") != "ACTIVE") {
        return "Recipient account is unavailable.";
    }
    if (!validAccountNumber(toNumber)) {
        return "Recipient account number failed checksum validation.";
    }
    if (db_.scalarInt(
            "SELECT COUNT(*) FROM cheques WHERE cheque_number=" +
            Database::quote(chequeNumber) + " AND account_id=" +
            field(*from, "account_id") + " AND status='ISSUED';") == 0) {
        return "Issued cheque not found for this account.";
    }
    if (recipientName.empty()) {
        return "Recipient name is required.";
    }

    postTransfer(number(*from, "account_id"), number(*to, "account_id"), amount,
                 "CHEQUE_TRANSFER", "Cheque " + chequeNumber + " to " +
                                         recipientName,
                 actor);
    db_.exec("UPDATE cheques SET status='USED' WHERE cheque_number=" +
             Database::quote(chequeNumber) + ";");
    audit(actor, "CHEQUE_TRANSFER",
          "Cheque " + chequeNumber + " to " + recipientName + " amount " +
              formatMoney(amount));
    return "Cheque transfer completed.";
}

std::string BankService::processNeft(const Session& staff) {
    if (staff.role != Role::Staff) {
        return "Not authorized.";
    }
    return "NEFT is approval-based now. Use Staff Approvals to review requests.";
}

std::string BankService::requestLoan(const Session& user, Money amount,
                                     const std::string& purpose,
                                     int termMonths) {
    if ((user.role != Role::User && user.role != Role::Staff) || amount <= 0 ||
        purpose.empty() || termMonths < 3 || termMonths > 60) {
        return "Enter a valid amount, purpose, and repayment term from 3 to 60 months.";
    }

    const auto score = db_.scalarInt(
        "SELECT credit_score FROM users WHERE user_id=" +
        std::to_string(user.userId) + ";");
    if (score < 400) {
        return "Loan request declined automatically: credit score is below 400.";
    }

    const auto limit = score >= 750 ? 500000000 : score >= 600 ? 200000000 : 50000000;
    if (amount > limit) {
        return "Requested amount exceeds the current credit-score limit of " +
               formatMoney(limit) + ".";
    }

    const Money monthlyEmi = (amount + termMonths - 1) / termMonths;
    Transaction transaction(db_);
    const auto timestamp = nowUtc();
    db_.exec(
        "INSERT INTO loans(user_id,amount,purpose,status,outstanding,term_months,"
        "monthly_emi,created_at) VALUES(" + std::to_string(user.userId) + "," +
        std::to_string(amount) + "," + Database::quote(purpose) +
        ",'PENDING'," + std::to_string(amount) + "," +
        std::to_string(termMonths) + "," + std::to_string(monthlyEmi) + "," +
        Database::quote(timestamp) + ");");

    const auto loanId = db_.lastInsertId();
    db_.exec(
        "INSERT INTO approval_requests(request_type,subject_user_id,amount,metadata,"
        "status,created_at,updated_at) VALUES('LOAN'," +
        std::to_string(user.userId) + "," + std::to_string(amount) + "," +
        Database::quote(std::to_string(loanId)) + ",'PENDING'," +
        Database::quote(timestamp) + "," + Database::quote(timestamp) + ");");

    const auto requestId = db_.lastInsertId();
    db_.exec("UPDATE loans SET request_id=" + std::to_string(requestId) +
             " WHERE loan_id=" + std::to_string(loanId) + ";");
    transaction.commit();

    audit(user, "LOAN_REQUESTED",
          "Loan #" + std::to_string(loanId) + "; " +
              std::to_string(termMonths) + " monthly instalments.");
    return "Loan request #" + std::to_string(loanId) +
           " submitted. Estimated EMI: " + formatMoney(monthlyEmi) +
           " each month.";
}

std::string BankService::disburseLoan(const Session& staff,
                                      std::int64_t loanId) {
    if (staff.role != Role::Staff) {
        return "Not authorized.";
    }

    const auto rows = db_.query("SELECT * FROM loans WHERE loan_id=" +
                                std::to_string(loanId) +
                                " AND status='APPROVED';");
    if (rows.empty()) {
        return "No approved loan awaits disbursal.";
    }

    const auto& loan = rows.front();
    const auto accounts = accountsForUser(number(loan, "user_id"));
    if (accounts.empty()) {
        return "Borrower has no active account.";
    }

    postTransfer(systemAccount("000000000004"),
                 number(accounts.front(), "account_id"), number(loan, "amount"),
                 "LOAN_DISBURSAL", "Loan #" + std::to_string(loanId), staff);
    db_.exec(
        "UPDATE loans SET status='DISBURSED',disbursed_at=" +
        Database::quote(nowUtc()) + ",next_due_date=date('now','+15 days') "
        "WHERE loan_id=" + std::to_string(loanId) + ";");
    audit(staff, "LOAN_DISBURSED", "Loan #" + std::to_string(loanId));
    return "Loan disbursed. First EMI of " +
           formatMoney(number(loan, "monthly_emi")) + " is due in 15 days.";
}

std::string BankService::requestClosure(const Session& user,
                                        const std::string& accountNumber) {
    const auto account = accountByNumber(accountNumber);
    if (user.role != Role::User || !account ||
        number(*account, "user_id") != user.userId) {
        return "Choose one of your accounts.";
    }

    const auto timestamp = nowUtc();
    db_.exec(
        "INSERT INTO approval_requests(request_type,subject_user_id,metadata,status,"
        "created_at,updated_at) VALUES('ACCOUNT_CLOSURE'," +
        std::to_string(user.userId) + "," + Database::quote(accountNumber) +
        ",'PENDING'," + Database::quote(timestamp) + "," +
        Database::quote(timestamp) + ");");
    audit(user, "CLOSURE_REQUESTED", accountNumber);
    return "Closure request submitted. The account must have a zero balance when Authority reviews it.";
}

std::string BankService::setAccountFrozen(const Session& authority,
                                          const std::string& accountNumber,
                                          bool frozen) {
    if (authority.role != Role::Authority) {
        return "Not authorized.";
    }

    const auto account = accountByNumber(accountNumber);
    if (!account) {
        return "Account not found.";
    }

    db_.exec("UPDATE accounts SET frozen=" + std::to_string(frozen ? 1 : 0) +
             " WHERE account_id=" + field(*account, "account_id") + ";");
    audit(authority, frozen ? "ACCOUNT_FROZEN" : "ACCOUNT_UNFROZEN",
          accountNumber);
    return frozen ? "Account frozen." : "Account unfrozen.";
}

std::string BankService::requestChequeBook(const Session& user,
                                           const std::string& accountNumber,
                                           int leaves,
                                           const std::string& address) {
    const auto account = accountByNumber(accountNumber);
    if ((user.role != Role::User && user.role != Role::Staff) || !account ||
        number(*account, "user_id") != user.userId || leaves < 1 || leaves > 100 ||
        address.empty()) {
        return "Enter a valid owned account, leaf count, and address.";
    }

    const auto metadata = accountNumber + "|" + std::to_string(leaves) + "|" + address;
    const auto timestamp = nowUtc();
    db_.exec(
        "INSERT INTO approval_requests(request_type,subject_user_id,metadata,status,"
        "created_at,updated_at) VALUES('CHEQUE_BOOK'," +
        std::to_string(user.userId) + "," + Database::quote(metadata) +
        ",'PENDING'," + Database::quote(timestamp) + "," +
        Database::quote(timestamp) + ");");
    audit(user, "CHEQUE_BOOK_REQUESTED",
          accountNumber + " leaves=" + std::to_string(leaves));
    return "Cheque book request sent to Staff for approval.";
}

std::string BankService::updateCheque(const Session& staff,
                                      const std::string& chequeNumber,
                                      const std::string& state) {
    if (staff.role != Role::Staff || (state != "USED" && state != "STOPPED")) {
        return "Only Staff may set cheque state to USED or STOPPED.";
    }
    if (db_.scalarInt(
            "SELECT COUNT(*) FROM cheques WHERE cheque_number=" +
            Database::quote(chequeNumber) + " AND status='ISSUED';") == 0) {
        return "Issued cheque not found.";
    }

    db_.exec("UPDATE cheques SET status=" + Database::quote(state) +
             " WHERE cheque_number=" + Database::quote(chequeNumber) + ";");
    audit(staff, "CHEQUE_" + state, chequeNumber);
    return "Cheque marked " + state + ".";
}

Rows BankService::activityLog(const Session& authority,
                              const std::string& role) const {
    if (authority.role != Role::Authority) {
        return {};
    }

    const std::string filter =
        role == "ALL" ? "" : " WHERE u.role=" + Database::quote(role);
    return db_.query(
        "SELECT ROW_NUMBER() OVER (ORDER BY a.audit_id DESC) AS s_no,a.created_at,"
        "COALESCE(ac.account_number,'SYSTEM') AS account_number,"
        "COALESCE(u.name,'System') AS name,a.details AS description "
        "FROM audit_logs a LEFT JOIN users u ON u.user_id=a.actor_user_id "
        "LEFT JOIN accounts ac ON ac.user_id=u.user_id" + filter +
        " ORDER BY a.audit_id DESC;");
}

Rows BankService::personLog(const Session& viewer, const std::string& role,
                            std::int64_t visibleId) const {
    if (viewer.role != Role::Authority && viewer.role != Role::Staff) {
        return {};
    }

    const auto userId = db_.scalarInt(
        "SELECT user_id FROM (SELECT user_id,ROW_NUMBER() OVER "
        "(PARTITION BY role ORDER BY user_id) AS visible_id FROM users "
        "WHERE role=" + Database::quote(role) +
        " AND status='ACTIVE') WHERE visible_id=" + std::to_string(visibleId) +
        ";",
        0);
    if (userId == 0) {
        return {};
    }

    return db_.query(
        "SELECT a.created_at,COALESCE(ac.account_number,'') AS account_number,"
        "COALESCE(u.name,'') AS name,a.details AS description FROM audit_logs a "
        "LEFT JOIN users u ON u.user_id=a.actor_user_id "
        "LEFT JOIN accounts ac ON ac.user_id=u.user_id WHERE a.actor_user_id=" +
        std::to_string(userId) + " ORDER BY a.audit_id DESC;");
}

Rows BankService::bankInfo(const Session& authority) const {
    if (authority.role != Role::Authority) {
        return {};
    }

    return db_.query(
        "SELECT ROW_NUMBER() OVER (PARTITION BY u.role ORDER BY u.user_id) "
        "AS visible_id,u.user_id,u.role,u.name,u.login,"
        "COALESCE(a.account_number,'') AS account_number,"
        "COALESCE(u.phone,'') AS phone,u.credit_score,"
        "COALESCE((SELECT SUM(CASE entry_side WHEN 'CREDIT' THEN amount "
        "ELSE -amount END) FROM ledger_entries WHERE account_id=a.account_id),0) "
        "AS balance FROM users u LEFT JOIN accounts a ON a.user_id=u.user_id "
        "WHERE u.role IN ('STAFF','USER') AND u.status='ACTIVE' "
        "ORDER BY u.role,u.user_id;");
}

Rows BankService::personInformation(const Session& authority,
                                    const std::string& role,
                                    std::int64_t visibleId) const {
    if (authority.role != Role::Authority) {
        return {};
    }

    const auto people = bankInfo(authority);
    for (const auto& person : people) {
        if (field(person, "role") == role &&
            number(person, "visible_id") == visibleId) {
            const auto rows = informationForAuthority(authority, role);
            Rows result;
            for (const auto& row : rows) {
                if (number(row, "user_id") == number(person, "user_id")) {
                    result.push_back(row);
                }
            }
            return result;
        }
    }
    return {};
}

Rows BankService::bankSummary(const Session& authority) const {
    if (authority.role != Role::Authority) {
        return {};
    }

    // The loan-fund account represents the bank's current available capital.
    // BANK_CAPITAL is initial capital and is excluded from operating totals.
    const auto loanFund = systemAccount("000000000004");
    const auto operationalDebit =
        "SELECT COALESCE(SUM(l.amount),0) FROM ledger_entries l "
        "JOIN bank_transactions t ON t.transaction_id=l.transaction_id "
        "WHERE l.entry_side='DEBIT' AND l.account_id IN "
        "(SELECT account_id FROM accounts WHERE user_id IS NULL) "
        "AND t.transaction_type NOT IN ('BANK_CAPITAL','OPENING_DEPOSIT');";
    const auto operationalCredit =
        "SELECT COALESCE(SUM(l.amount),0) FROM ledger_entries l "
        "JOIN bank_transactions t ON t.transaction_id=l.transaction_id "
        "WHERE l.entry_side='CREDIT' AND l.account_id IN "
        "(SELECT account_id FROM accounts WHERE user_id IS NULL) "
        "AND t.transaction_type NOT IN ('BANK_CAPITAL','OPENING_DEPOSIT');";

    Row summary;
    summary["money_in_bank"] = std::to_string(db_.scalarInt(
        "SELECT COALESCE(SUM(CASE entry_side WHEN 'CREDIT' THEN amount "
        "ELSE -amount END),0) FROM ledger_entries WHERE account_id=" +
        std::to_string(loanFund) + ";"));
    summary["bank_balance"] = summary["money_in_bank"];
    summary["total_transactions"] = std::to_string(db_.scalarInt(
        "SELECT COUNT(*) FROM bank_transactions WHERE transaction_type NOT IN "
        "('BANK_CAPITAL','OPENING_DEPOSIT');"));
    summary["total_employees"] = std::to_string(db_.scalarInt(
        "SELECT COUNT(*) FROM users WHERE role='STAFF' AND status='ACTIVE';"));
    summary["total_debits"] = std::to_string(db_.scalarInt(operationalDebit));
    summary["total_credits"] = std::to_string(db_.scalarInt(operationalCredit));
    summary["loans_disbursed"] = std::to_string(db_.scalarInt(
        "SELECT COALESCE(SUM(amount),0) FROM loans WHERE disbursed_at IS NOT NULL;"));
    summary["authority_count"] = std::to_string(db_.scalarInt(
        "SELECT COUNT(*) FROM users WHERE role='AUTHORITY' AND status='ACTIVE';"));
    summary["staff_count"] = std::to_string(db_.scalarInt(
        "SELECT COUNT(*) FROM users WHERE role='STAFF' AND status='ACTIVE';"));
    summary["user_count"] = std::to_string(db_.scalarInt(
        "SELECT COUNT(*) FROM users WHERE role='USER' AND status='ACTIVE';"));
    return {summary};
}

Rows BankService::loanDetails(const Session& viewer,
                              std::optional<std::int64_t> userId) const {
    if (viewer.role == Role::User || viewer.role == Role::Staff) {
        if (!userId) {
            userId = viewer.userId;
        }
    } else if (viewer.role != Role::Authority) {
        return {};
    }

    return db_.query(
        "SELECT l.loan_id,l.user_id,u.name,u.login,l.amount,l.status,l.outstanding,"
        "l.term_months,l.monthly_emi,COALESCE(l.next_due_date,'Not scheduled') "
        "AS next_due_date,CASE WHEN l.next_due_date IS NULL THEN 'Not scheduled' "
        "WHEN date(l.next_due_date)<date('now') THEN 'OVERDUE' "
        "WHEN date(l.next_due_date)=date('now') THEN 'DUE TODAY' ELSE 'Upcoming' END "
        "AS payment_status,l.purpose,l.created_at,COALESCE((SELECT SUM(amount) "
        "FROM loan_payments p WHERE p.loan_id=l.loan_id),0) AS paid "
        "FROM loans l JOIN users u ON u.user_id=l.user_id WHERE l.user_id=" +
        std::to_string(*userId) + " ORDER BY l.loan_id;");
}

Rows BankService::emiSchedule(const Session& viewer) const {
    if (viewer.role != Role::User && viewer.role != Role::Staff) {
        return {};
    }

    const auto userId = std::to_string(viewer.userId);
    return db_.query(
        "WITH RECURSIVE schedule AS ("
        "SELECT l.loan_id,l.user_id,l.status,l.term_months,l.monthly_emi,"
        "l.outstanding,l.next_due_date,1 AS emi_no FROM loans l WHERE l.user_id=" +
        userId + " AND l.status!='PAID' "
        "UNION ALL SELECT loan_id,user_id,status,term_months,monthly_emi,"
        "outstanding,next_due_date,emi_no+1 FROM schedule WHERE emi_no<term_months) "
        "SELECT ROW_NUMBER() OVER (ORDER BY loan_id,emi_no) AS s_no,loan_id,emi_no,"
        "status,monthly_emi AS emi_amount,CASE WHEN next_due_date IS NULL THEN "
        "'Not scheduled' ELSE date(next_due_date,'+'||((emi_no-1)*15)||' days') END "
        "AS due_date,CASE WHEN status!='DISBURSED' THEN status "
        "WHEN COALESCE((SELECT SUM(amount) FROM loan_payments p WHERE "
        "p.loan_id=schedule.loan_id),0)>=emi_no*monthly_emi THEN 'PAID' "
        "WHEN MIN(monthly_emi,MAX(0,COALESCE((SELECT SUM(amount) FROM loan_payments p "
        "WHERE p.loan_id=schedule.loan_id),0)-(emi_no-1)*monthly_emi))>0 THEN 'PARTIAL' "
        "WHEN date(next_due_date,'+'||((emi_no-1)*15)||' days')<date('now') THEN 'OVERDUE' "
        "ELSE 'DUE' END AS payment_status,MIN(monthly_emi,MAX(0,COALESCE("
        "(SELECT SUM(amount) FROM loan_payments p WHERE p.loan_id=schedule.loan_id),0)"
        "-(emi_no-1)*monthly_emi)) AS paid,outstanding FROM schedule "
        "ORDER BY loan_id,emi_no;");
}

Rows BankService::approvedLoans(const Session& staff) const {
    if (staff.role != Role::Staff) {
        return {};
    }
    return db_.query(
        "SELECT ROW_NUMBER() OVER (ORDER BY l.loan_id) AS s_no,l.loan_id,u.name,"
        "l.status,l.monthly_emi,l.next_due_date,l.outstanding,COALESCE((SELECT "
        "SUM(amount) FROM loan_payments p WHERE p.loan_id=l.loan_id),0) AS paid "
        "FROM loans l JOIN users u ON u.user_id=l.user_id "
        "WHERE l.status='APPROVED' ORDER BY l.loan_id;");
}

Rows BankService::chequeRequests(const Session& viewer) const {
    if (viewer.role != Role::Staff && viewer.role != Role::User) {
        return {};
    }
    const auto filter = viewer.role == Role::User
                            ? " AND r.subject_user_id=" +
                                  std::to_string(viewer.userId)
                            : "";
    return db_.query(
        "SELECT r.request_id,r.request_type,u.name,r.metadata,r.status,r.created_at "
        "FROM approval_requests r JOIN users u ON u.user_id=r.subject_user_id "
        "WHERE r.request_type='CHEQUE_BOOK'" + filter +
        " ORDER BY r.request_id DESC;");
}

Rows BankService::chequesFor(const Session& viewer) const {
    if (viewer.role != Role::Staff && viewer.role != Role::User) {
        return {};
    }
    const auto filter = viewer.role == Role::User
                            ? " WHERE a.user_id=" + std::to_string(viewer.userId)
                            : "";
    return db_.query(
        "SELECT c.cheque_number,c.status,c.created_at,a.account_number,u.name "
        "FROM cheques c JOIN accounts a ON a.account_id=c.account_id "
        "JOIN users u ON u.user_id=a.user_id" + filter +
        " ORDER BY c.cheque_id DESC;");
}

std::string BankService::requestAddMoney(const Session& user,
                                         const std::string& accountNumber,
                                         Money amount) {
    const auto account = accountByNumber(accountNumber);
    if ((user.role != Role::User && user.role != Role::Staff) || !account ||
        number(*account, "user_id") != user.userId || amount <= 0) {
        return "Choose your account and enter a positive amount.";
    }

    const auto timestamp = nowUtc();
    db_.exec(
        "INSERT INTO approval_requests(request_type,subject_user_id,amount,metadata,"
        "status,created_at,updated_at) VALUES('ADD_MONEY'," +
        std::to_string(user.userId) + "," + std::to_string(amount) + "," +
        Database::quote(accountNumber) + ",'PENDING'," +
        Database::quote(timestamp) + "," + Database::quote(timestamp) + ");");
    audit(user, "ADD_MONEY_REQUESTED",
          formatMoney(amount) + " to " + accountNumber);
    return "Add-money request sent to Staff.";
}

std::string BankService::approveAddMoney(const Session& staff,
                                         std::int64_t requestId, bool approve) {
    if (staff.role != Role::Staff) {
        return "Not authorized.";
    }

    const auto rows = db_.query(
        "SELECT * FROM approval_requests WHERE request_id=" +
        std::to_string(requestId) +
        " AND request_type='ADD_MONEY' AND status='PENDING';");
    if (rows.empty()) {
        return "Add-money request not found.";
    }

    const auto& request = rows.front();
    if (approve) {
        const auto account = accountByNumber(field(request, "metadata"));
        if (!account) {
            return "Account no longer exists.";
        }
        postTransfer(systemAccount("000000000001"),
                     number(*account, "account_id"), number(request, "amount"),
                     "ADD_MONEY", "Staff-approved deposit", staff);
    }

    const auto status = approve ? "APPROVED" : "REJECTED";
    db_.exec(
        "UPDATE approval_requests SET status=" + Database::quote(status) +
        ",staff_reviewer_id=" + std::to_string(staff.userId) +
        ",updated_at=" + Database::quote(nowUtc()) +
        " WHERE request_id=" + std::to_string(requestId) + ";");
    audit(staff, approve ? "ADD_MONEY_APPROVED" : "ADD_MONEY_REJECTED",
          "Request #" + std::to_string(requestId));
    return approve ? "Money added to the account." : "Add-money request rejected.";
}

std::string BankService::payEmi(const Session& payer, std::int64_t loanId,
                                Money amount) {
    const auto rows = db_.query(
        "SELECT * FROM loans WHERE loan_id=" + std::to_string(loanId) +
        " AND status='DISBURSED' AND outstanding>0;");
    if (rows.empty()) {
        return "No active loan with outstanding balance found.";
    }

    const auto& loan = rows.front();
    if (number(loan, "user_id") != payer.userId) {
        return "This loan does not belong to the signed-in account.";
    }

    const auto paid = std::min(amount, number(loan, "outstanding"));
    if (paid <= 0) {
        return "Payment must be positive.";
    }

    const auto previousPaid = db_.scalarInt(
        "SELECT COALESCE(SUM(amount),0) FROM loan_payments WHERE loan_id=" +
        std::to_string(loanId) + ";");
    const auto accounts = accountsForUser(payer.userId);
    if (accounts.empty()) {
        return "No active repayment account found.";
    }
    const auto repaymentAccount = number(accounts.front(), "account_id");
    if (balance(repaymentAccount) < paid) {
        return "Insufficient balance for EMI payment.";
    }

    const auto previousScore = db_.scalarInt(
        "SELECT credit_score FROM users WHERE user_id=" +
        std::to_string(payer.userId) + ";");
    const auto nextScore = std::min<std::int64_t>(850, previousScore + 10);

    postTransfer(repaymentAccount, systemAccount("000000000004"), paid,
                 "LOAN_REPAYMENT", "EMI payment", payer);

    const bool completed = paid >= number(loan, "outstanding");
    const auto emiAmount = number(loan, "monthly_emi");
    const auto installmentsBefore = emiAmount > 0 ? previousPaid / emiAmount : 0;
    const auto installmentsAfter =
        emiAmount > 0 ? (previousPaid + paid) / emiAmount : 0;
    const auto installmentsCovered =
        std::max<std::int64_t>(0, installmentsAfter - installmentsBefore);

    db_.exec(
        "UPDATE loans SET outstanding=outstanding-" + std::to_string(paid) +
        ",status=CASE WHEN outstanding-" + std::to_string(paid) +
        "<=0 THEN 'PAID' ELSE status END,next_due_date=CASE WHEN outstanding-" +
        std::to_string(paid) + "<=0 THEN NULL WHEN " +
        std::to_string(installmentsCovered) + ">0 THEN date(COALESCE(next_due_date,"
        "date('now')),'+" + std::to_string(installmentsCovered * 15) +
        " days') ELSE next_due_date END WHERE loan_id=" +
        std::to_string(loanId) + ";");

    db_.exec(
        "INSERT INTO loan_payments(loan_id,payer_user_id,amount,principal_component,"
        "interest_component,paid_at,credit_score_before,credit_score_after) VALUES(" +
        std::to_string(loanId) + "," + std::to_string(payer.userId) + "," +
        std::to_string(paid) + "," + std::to_string(paid) + ",0," +
        Database::quote(nowUtc()) + "," + std::to_string(previousScore) + "," +
        std::to_string(nextScore) + ");UPDATE users SET credit_score=" +
        std::to_string(nextScore) + " WHERE user_id=" +
        std::to_string(payer.userId) + ";");

    audit(payer, "EMI_PAID",
          "Loan payment completed; score " + std::to_string(previousScore) +
              " -> " + std::to_string(nextScore));

    if (completed) {
        return "Loan fully repaid. Credit score increased to " +
               std::to_string(nextScore) + ".";
    }
    if (installmentsCovered > 0) {
        return "Payment received. Your next due date moved forward by " +
               std::to_string(installmentsCovered * 15) + " days.";
    }
    return "Partial payment received. Pay the remaining EMI amount before the due date.";
}

Rows BankService::history(std::int64_t accountId) const {
    return db_.query(
        "SELECT t.created_at,t.transaction_ref,t.transaction_type,l.entry_side,"
        "l.amount,t.memo FROM ledger_entries l JOIN bank_transactions t ON "
        "t.transaction_id=l.transaction_id WHERE l.account_id=" +
        std::to_string(accountId) + " ORDER BY l.entry_id DESC;");
}

Rows BankService::auditLog(int limit) const {
    return db_.query(
        "SELECT ROW_NUMBER() OVER (ORDER BY a.audit_id DESC) AS s_no,a.created_at,"
        "COALESCE(u.name,'System') AS name,COALESCE(ac.account_number,'SYSTEM') "
        "AS account_number,a.details AS description FROM audit_logs a "
        "LEFT JOIN users u ON u.user_id=a.actor_user_id "
        "LEFT JOIN accounts ac ON ac.user_id=u.user_id "
        "ORDER BY a.audit_id DESC LIMIT " + std::to_string(limit) + ";");
}

Rows BankService::fraudFlags() const {
    return db_.query(
        "SELECT f.flag_id,f.created_at,t.transaction_ref,f.reason,f.status "
        "FROM fraud_flags f JOIN bank_transactions t ON "
        "t.transaction_id=f.transaction_id ORDER BY f.flag_id DESC;");
}

std::string BankService::exportHistory(std::int64_t accountId,
                                       const std::string& path) const {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    std::ofstream output(path);
    if (!output) {
        return "Could not create export file.";
    }

    auto csv = [](std::string value) {
        std::string result = "\"";
        for (const char character : value) {
            result += character;
            if (character == '\"') {
                result += '\"';
            }
        }
        return result + "\"";
    };

    output << "timestamp,reference,type,side,amount_paise,memo\n";
    for (const auto& row : history(accountId)) {
        output << csv(field(row, "created_at")) << ','
               << csv(field(row, "transaction_ref")) << ','
               << csv(field(row, "transaction_type")) << ','
               << csv(field(row, "entry_side")) << ','
               << field(row, "amount") << ',' << csv(field(row, "memo")) << '\n';
    }
    return "History exported to " + path;
}

std::string BankService::updateProfile(
    const Session& actor, const std::string& name,
    const std::string& email, const std::string& phone,
    const std::string& pan, const std::string& aadhaar,
    const std::string& newPassword, const std::string& authorityKey) {
    if (name.empty()) {
        return "Name cannot be empty.";
    }
    if (actor.role == Role::Authority &&
        authorityKey != security_.config().authorityRegistrationKey) {
        return "Authority key confirmation failed.";
    }
    if (!userByLogin(actor.login)) {
        return "Account not found.";
    }

    std::string update = "UPDATE users SET name=" + Database::quote(name) +
                         ",email=" + Database::quote(email) +
                         ",phone=" + Database::quote(phone);
    if (!newPassword.empty()) {
        if (newPassword.size() < 8) {
            return "New password must be at least 8 characters.";
        }
        const auto salt = security_.randomHex(16);
        update += ",password_hash=" +
                  Database::quote(security_.hashPassword(newPassword, salt)) +
                  ",password_salt=" + Database::quote(salt);
    }
    update += " WHERE user_id=" + std::to_string(actor.userId) + ";";
    db_.exec(update);

    if (!pan.empty() || !aadhaar.empty()) {
        const auto profile = db_.query(
            "SELECT user_id,aadhaar_enc,pan_enc FROM customer_profiles WHERE user_id=" +
            std::to_string(actor.userId) + ";");
        const auto oldAadhaar =
            profile.empty() ? "" : field(profile.front(), "aadhaar_enc");
        const auto oldPan = profile.empty() ? "" : field(profile.front(), "pan_enc");
        const auto newAadhaar = aadhaar.empty() ? oldAadhaar : aadhaar;
        const auto newPan = pan.empty() ? oldPan : pan;

        if (profile.empty()) {
            db_.exec(
                "INSERT INTO customer_profiles(user_id,aadhaar_enc,pan_enc) VALUES(" +
                std::to_string(actor.userId) + "," + Database::quote(newAadhaar) +
                "," + Database::quote(newPan) + ");");
        } else {
            db_.exec(
                "UPDATE customer_profiles SET aadhaar_enc=" +
                Database::quote(newAadhaar) + ",pan_enc=" +
                Database::quote(newPan) + " WHERE user_id=" +
                std::to_string(actor.userId) + ";");
        }
    }

    audit(actor, "PROFILE_UPDATED", "Profile settings changed.");
    return "Profile settings updated. Account number was not changed.";
}

std::string BankService::updateSetting(const Session& actor,
                                       const std::string& fieldName,
                                       const std::string& value,
                                       const std::string& authorityKey) {
    if (value.empty()) {
        return "A value is required.";
    }
    if (actor.role == Role::Authority &&
        authorityKey != security_.config().authorityRegistrationKey) {
        return "Authority key confirmation failed.";
    }

    if (fieldName == "name" || fieldName == "email" || fieldName == "phone") {
        db_.exec("UPDATE users SET " + fieldName + "=" + Database::quote(value) +
                 " WHERE user_id=" + std::to_string(actor.userId) + ";");
    } else if (fieldName == "password") {
        if (value.size() < 8) {
            return "Password must be at least 8 characters.";
        }
        const auto salt = security_.randomHex(16);
        db_.exec(
            "UPDATE users SET password_hash=" +
            Database::quote(security_.hashPassword(value, salt)) +
            ",password_salt=" + Database::quote(salt) +
            " WHERE user_id=" + std::to_string(actor.userId) + ";");
    } else if (fieldName == "pan" || fieldName == "aadhaar") {
        const auto profile = db_.query(
            "SELECT user_id FROM customer_profiles WHERE user_id=" +
            std::to_string(actor.userId) + ";");
        const auto column = fieldName == "aadhaar" ? "aadhaar_enc" : "pan_enc";
        if (profile.empty()) {
            db_.exec(
                "INSERT INTO customer_profiles(user_id,aadhaar_enc,pan_enc) VALUES(" +
                std::to_string(actor.userId) + "," +
                Database::quote(fieldName == "aadhaar" ? value : "") + "," +
                Database::quote(fieldName == "pan" ? value : "") + ");");
        } else {
            db_.exec(std::string("UPDATE customer_profiles SET ") + column + "=" +
                     Database::quote(value) + " WHERE user_id=" +
                     std::to_string(actor.userId) + ";");
        }
    } else {
        return "That setting cannot be changed.";
    }

    audit(actor, "SETTING_CHANGED", fieldName + " updated");
    return "The selected setting was updated.";
}

} // namespace jdn

