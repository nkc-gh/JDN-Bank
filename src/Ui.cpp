/**
 * @file Ui.cpp
 * @brief Terminal screens and input handling for JDN Bank.
 */
#include "Ui.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <termios.h>
#include <unistd.h>

namespace jdn {
namespace {

// Colour names are kept as empty strings for the plain terminal version.
// They are named so colours can be enabled later without changing menu code.
constexpr const char* red = "";
constexpr const char* green = "";
constexpr const char* reset = "";
constexpr const char* bold = "";

/** @brief Read one text field from a database row. */
std::string valueOf(const Row& row, const std::string& key) {
    const auto found = row.find(key);
    return found == row.end() ? "" : found->second;
}

/** @brief Read one integer field from a database row. */
std::int64_t numberOf(const Row& row, const std::string& key) {
    try {
        return std::stoll(valueOf(row, key));
    } catch (...) {
        return 0;
    }
}

/** @brief Return an uppercase copy used for role selection. */
std::string uppercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::toupper(character));
                   });
    return text;
}

/** @brief Count visible UTF-8 code points for simple table alignment. */
std::size_t displayWidth(const std::string& text) {
    std::size_t width = 0;
    for (const unsigned char character : text) {
        if ((character & 0xC0U) != 0x80U) {
            ++width;
        }
    }
    return width;
}

/** @brief Add spaces on the right until a table column reaches its width. */
std::string padRight(const std::string& text, std::size_t width) {
    if (width <= displayWidth(text)) {
        return text;
    }
    return text + std::string(width - displayWidth(text), ' ');
}

/** @brief Draw a consistent heading for every terminal screen. */
void printHeading(const std::string& title) {
    if (::isatty(STDOUT_FILENO)) {
        std::cout << "\033[2J\033[H";
    }

    std::cout << "+----------------------------------------------------------------------+\n"
              << "|                              JDN BANK                                |\n"
              << "+----------------------------------------------------------------------+\n"
              << "  " << title << "\n\n";
}

/** @brief Replace a money field in a row with display-friendly rupees. */
void formatMoneyField(Row& row, const std::string& key) {
    row[key] = formatMoney(numberOf(row, key));
}

} // namespace

Ui::Ui(BankService& service) : service_(service) {}

std::string Ui::ask(const std::string& label, bool required) const {
    std::string value;
    do {
        std::cout << "  " << label;
        if (!std::getline(std::cin, value)) {
            // Returning an empty value lets choose() and the caller stop cleanly
            // when input is piped or the terminal is closed.
            return "";
        }

        value = trim(value);
        if (required && value.empty()) {
            std::cout << red << "  Required.\n" << reset;
        }
    } while (required && value.empty());

    return value;
}

std::string Ui::askSecret(const std::string& label) const {
    std::cout << "  " << label;

    // Password hiding is only possible when stdin is an actual terminal.
    if (!::isatty(STDIN_FILENO)) {
        std::string value;
        if (!std::getline(std::cin, value)) {
            return "";
        }
        return trim(value);
    }

    termios original{};
    termios hidden{};
    tcgetattr(STDIN_FILENO, &original);
    hidden = original;
    hidden.c_lflag &= static_cast<tcflag_t>(~static_cast<tcflag_t>(ECHO));
    tcsetattr(STDIN_FILENO, TCSANOW, &hidden);

    std::string value;
    std::getline(std::cin, value);
    tcsetattr(STDIN_FILENO, TCSANOW, &original);
    std::cout << "\n";
    return trim(value);
}

int Ui::choose(const std::string& title,
               const std::vector<std::string>& options) const {
    std::cout << bold << "  " << title << reset << "\n";
    for (std::size_t index = 0; index < options.size(); ++index) {
        std::cout << "    [" << index + 1 << "] " << options[index] << "\n";
    }

    for (;;) {
        const std::string input = ask("Choose: ");
        if (!std::cin) {
            return 0;
        }

        try {
            const int choice = std::stoi(input);
            if (choice > 0 && choice <= static_cast<int>(options.size())) {
                return choice;
            }
        } catch (...) {
            // The message below is clearer to a beginner than an exception.
        }

        std::cout << red << "  Invalid choice.\n" << reset;
    }
}

void Ui::pause() const {
    std::cout << "\n  Press Enter to continue...";
    std::string ignored;
    std::getline(std::cin, ignored);
}

void Ui::printRows(const Rows& rows,
                   const std::vector<std::string>& columns) const {
    if (rows.empty()) {
        std::cout << "  No records found.\n";
        return;
    }

    // First calculate a suitable width for every requested column.
    std::vector<std::size_t> widths;
    for (const auto& column : columns) {
        widths.push_back(std::max<std::size_t>(8, displayWidth(column)));
    }
    for (const auto& row : rows) {
        for (std::size_t column = 0; column < columns.size(); ++column) {
            widths[column] = std::min<std::size_t>(
                36, std::max(widths[column],
                             displayWidth(valueOf(row, columns[column]))));
        }
    }

    std::cout << "  ";
    for (std::size_t column = 0; column < columns.size(); ++column) {
        std::cout << padRight(columns[column], widths[column]) << "  ";
    }
    std::cout << "\n  ";
    for (const auto width : widths) {
        std::cout << std::string(width, '-') << "  ";
    }

    for (const auto& row : rows) {
        std::cout << "\n  ";
        for (std::size_t column = 0; column < columns.size(); ++column) {
            std::string text = valueOf(row, columns[column]);
            if (displayWidth(text) > widths[column]) {
                text = text.substr(0, widths[column] > 3 ? widths[column] - 3 : 0) +
                       "...";
            }
            std::cout << padRight(text, widths[column]) << "  ";
        }
    }
    std::cout << "\n";
}

void Ui::printVertical(const Row& row,
                       const std::vector<std::string>& fields) const {
    for (const auto& field : fields) {
        std::cout << "  " << std::left << std::setw(20) << field << ": "
                  << valueOf(row, field) << "\n";
    }
}

void Ui::welcome() {
    printHeading("Welcome");
    std::cout << "  Welcome to JDN Bank.\n\n";
}

void Ui::run() {
    for (;;) {
        welcome();
        const int choice = choose("JDN Bank", {"Login", "Register", "Exit"});
        if (choice == 0 || choice == 3) {
            printHeading("Goodbye");
            std::cout << "  Thank you for using JDN Bank.\n";
            return;
        }

        if (choice == 2) {
            registerFlow();
        } else {
            loginFlow(Role::User);
        }
    }
}

void Ui::loginFlow(Role /*role*/) {
    printHeading("Login");
    const auto login = ask("User ID: ");
    const auto password = askSecret("Password: ");

    std::string message;
    std::optional<Session> session;
    for (const auto role : {Role::Authority, Role::Staff, Role::User}) {
        session = service_.login(role, login, password, message);
        if (session) {
            break;
        }
    }

    if (!session) {
        std::cout << red << "\n  " << message << reset << "\n";
        pause();
        return;
    }

    std::cout << green << "\n  Welcome, " << session->name << "." << reset << "\n";
    pause();

    if (session->role == Role::Authority) {
        authorityMenu(*session);
    } else if (session->role == Role::Staff) {
        staffMenu(*session);
    } else {
        userMenu(*session);
    }
}

void Ui::registerFlow() {
    printHeading("Register");
    const int type = choose("Registration type",
                            {"Authority", "Staff", "User", "Back", "Exit"});
    if (type == 0 || type == 4) {
        return;
    }
    if (type == 5) {
        std::exit(0);
    }

    const auto name = ask("Full name: ");
    const auto mobile = ask("Mobile number: ");
    const auto dob = ask("Date of birth: ");
    const auto email = ask("Email: ");
    const auto address = ask("Address: ");
    const auto pan = askSecret("PAN: ");
    const auto aadhaar = askSecret("Aadhaar: ");
    const auto login = ask("User ID: ");
    const auto password = askSecret("Password (minimum 8 characters): ");

    std::string result;
    try {
        if (type == 1) {
            result = service_.registerAuthorityFull(
                name, mobile, dob, email, address, pan, aadhaar, login, password,
                askSecret("Authority key: "));
        } else if (type == 2) {
            result = service_.requestStaffRegistrationFull(
                name, mobile, dob, email, address, pan, aadhaar, login, password);
        } else {
            result = service_.requestUserRegistrationFull(
                name, mobile, dob, email, address, pan, aadhaar, login, password);
        }
    } catch (const std::exception& error) {
        result = std::string("Registration failed: ") + error.what();
    }

    std::cout << "\n  " << result << "\n";
    pause();
}

void Ui::showInfo(const Session& session, bool authorityView) {
    printHeading("Information");
    auto rows = authorityView
                    ? service_.informationForAuthority(session, "AUTHORITY")
                    : service_.ownInformation(session);

    for (auto& row : rows) {
        formatMoneyField(row, "balance");
        if (authorityView) {
            printVertical(row, {"name", "phone", "dob", "email", "address",
                                "pan", "aadhaar", "login", "account_number"});
        } else {
            printVertical(row, {"name", "phone", "dob", "email", "address",
                                "pan", "aadhaar", "login", "account_number",
                                "credit_score", "balance"});
        }
    }
    pause();
}

void Ui::showHistory(const Session& session) {
    printHeading("Transaction history");
    const auto accounts = service_.accountsForUser(session.userId, true);
    if (accounts.empty()) {
        std::cout << "  No account found.\n";
        pause();
        return;
    }

    auto rows = service_.history(numberOf(accounts.front(), "account_id"));
    for (auto& row : rows) {
        formatMoneyField(row, "amount");
    }
    printRows(rows, {"created_at", "transaction_ref", "transaction_type",
                     "entry_side", "amount", "memo"});
    pause();
}

void Ui::settingsMenu(const Session& session) {
    const std::vector<std::string> fields = {
        "name", "email", "phone", "pan", "aadhaar", "password"};

    for (;;) {
        printHeading("Settings");
        const int choice = choose("Change", {"Name", "Email", "Mobile", "PAN",
                                              "Aadhaar", "Password", "Back"});
        if (choice == 0 || choice >= 7) {
            return;
        }

        const auto index = static_cast<std::size_t>(choice - 1);
        std::string value;
        if (choice == 4) {
            value = askSecret("New PAN: ");
        } else if (choice == 5) {
            value = askSecret("New Aadhaar: ");
        } else if (choice == 6) {
            value = askSecret("New password: ");
        } else {
            value = ask("New " + fields[index] + ": ");
        }

        const auto authorityKey = session.role == Role::Authority
                                      ? askSecret("Authority key: ")
                                      : "";
        std::cout << service_.updateSetting(session, fields[index], value,
                                            authorityKey)
                  << "\n";
        pause();
    }
}

void Ui::transferMenu(const Session& session) {
    printHeading("Transfer money");
    const auto accounts = service_.accountsForUser(session.userId);
    if (accounts.empty()) {
        std::cout << "  No active account.\n";
        pause();
        return;
    }

    const int type = choose("Transfer type", {"IMPS", "NEFT", "RTGS", "Cheque",
                                               "Back"});
    if (type == 0 || type == 5) {
        return;
    }

    const auto recipient = ask("Recipient account number: ");
    const auto amount = parseMoney(ask("Amount: "));
    const auto sender = valueOf(accounts.front(), "account_number");

    std::string result;
    if (type == 4) {
        result = service_.transferByCheque(
            session, sender, recipient, ask("Cheque number: "), amount,
            ask("Recipient name: "));
    } else {
        const auto rail = type == 1 ? "IMPS" : type == 2 ? "NEFT" : "RTGS";
        result = service_.transfer(session, sender, recipient, amount, rail,
                                   "Transfer to " + recipient);
    }

    std::cout << result << "\n";
    pause();
}

void Ui::applyMenu(const Session& session) {
    printHeading("Applications");
    const int type = choose("Application", {"Cheque book", "Loan", "Add money",
                                             "Passbook", "Back"});
    if (type == 0 || type == 5) {
        return;
    }

    const auto accounts = service_.accountsForUser(session.userId);
    std::string result;
    if (type == 1) {
        result = accounts.empty()
                     ? "No account found."
                     : service_.requestChequeBook(
                           session, valueOf(accounts.front(), "account_number"),
                           std::stoi(ask("Leaves (1-100): ")),
                           ask("Delivery address: "));
    } else if (type == 2) {
        result = service_.requestLoan(session, parseMoney(ask("Requested amount: ")),
                                      ask("Purpose: "),
                                      std::stoi(ask("Term in months: ")));
    } else if (type == 3) {
        result = accounts.empty()
                     ? "No account found."
                     : service_.requestAddMoney(
                           session, valueOf(accounts.front(), "account_number"),
                           parseMoney(ask("Amount: ")));
    } else {
        showHistory(session);
        return;
    }

    std::cout << result << "\n";
    pause();
}

void Ui::loanMenu(const Session& session) {
    printHeading("Loans and EMI");
    auto rows = service_.emiSchedule(session);

    // Keep the original paise values in hidden helper fields so payment logic
    // can use exact integers after the visible fields are formatted.
    for (auto& row : rows) {
        row["emi_paise"] = valueOf(row, "emi_amount");
        row["paid_paise"] = valueOf(row, "paid");
        formatMoneyField(row, "emi_amount");
        formatMoneyField(row, "paid");
    }

    printRows(rows, {"s_no", "loan_id", "emi_no", "status", "emi_amount",
                     "due_date", "payment_status", "paid"});
    if (rows.empty()) {
        pause();
        return;
    }

    const auto input = ask("EMI S.no to pay, or 0: ");
    if (input == "0" || input.empty()) {
        pause();
        return;
    }

    try {
        const auto selected = std::stoll(input);
        if (selected <= 0 || selected > static_cast<std::int64_t>(rows.size())) {
            std::cout << "Invalid EMI S.no.\n";
            pause();
            return;
        }

        auto& row = rows[static_cast<std::size_t>(selected - 1)];
        if (valueOf(row, "status") != "DISBURSED") {
            std::cout << "This loan is not disbursed yet.\n";
        } else if (valueOf(row, "payment_status") == "PAID") {
            std::cout << "That EMI is already paid.\n";
        } else {
            const int paymentType = choose("Payment type", {"EMI", "Advance",
                                                               "Back"});
            if (paymentType < 3 && paymentType != 0) {
                Money paid = 0;
                if (paymentType == 1) {
                    paid = std::stoll(valueOf(row, "emi_paise")) -
                           std::stoll(valueOf(row, "paid_paise"));
                } else {
                    paid = parseMoney(ask("Advance amount: "));
                }

                if (paid > 0) {
                    std::cout << service_.payEmi(
                                     session, numberOf(row, "loan_id"), paid)
                              << "\n";
                } else {
                    std::cout << "Enter a positive payment amount.\n";
                }
            }
        }
    } catch (...) {
        std::cout << "Invalid EMI S.no.\n";
    }
    pause();
}

void Ui::chequeMenu(const Session& session) {
    printHeading("Cheque requests");
    printRows(service_.chequeRequests(session), {"request_id", "name", "metadata",
                                                  "status", "created_at"});
    printRows(service_.chequesFor(session), {"cheque_number", "status",
                                             "account_number", "name", "created_at"});
    pause();
}

void Ui::authorityMenu(const Session& session) {
    for (;;) {
        printHeading("Authority dashboard");
        const int choice = choose("Authority services", {"Info", "Logs", "Approvals",
                                                          "Settings", "Refresh",
                                                          "Sign out"});
        if (choice == 0 || choice == 6) {
            return;
        }

        try {
            if (choice == 1) {
                showInfo(session, true);
            } else if (choice == 2) {
                printHeading("Logs");
                const int logType = choose("Log type", {"Bank log", "Info log", "Back"});
                if (logType == 1) {
                    auto summary = service_.bankSummary(session).front();
                    formatMoneyField(summary, "bank_balance");
                    printVertical(summary, {"bank_balance", "total_transactions",
                                            "total_employees"});
                    printRows(service_.auditLog(500), {"s_no", "name",
                                                        "account_number", "description"});
                    pause();
                } else if (logType == 2) {
                    const auto people = service_.bankInfo(session);
                    printRows(people, {"visible_id", "role", "name",
                                       "account_number", "login"});
                    const auto role = uppercase(ask("Role (STAFF or USER): "));
                    const auto id = ask("S.no, or 0: ");
                    if (id != "0" && !id.empty()) {
                        const auto visibleId = std::stoll(id);
                        for (auto row : service_.personInformation(session, role,
                                                                    visibleId)) {
                            formatMoneyField(row, "balance");
                            printVertical(row, {"name", "phone", "dob", "email",
                                                "address", "pan", "aadhaar", "login",
                                                "account_number", "credit_score", "balance"});
                        }
                        printRows(service_.personLog(session, role, visibleId),
                                  {"created_at", "name", "account_number",
                                   "description"});
                    }
                    pause();
                }
            } else if (choice == 3) {
                auto requests = service_.requestsFor(session);
                for (auto& row : requests) {
                    formatMoneyField(row, "amount");
                }
                printRows(requests, {"request_id", "request_type", "requester_name",
                                     "requester_login", "amount", "status", "created_at"});
                const auto id = ask("Request ID, or 0: ");
                if (id != "0" && !id.empty()) {
                    const int decision = choose("Decision", {"Approve", "Reject", "Back"});
                    if (decision < 3 && decision != 0) {
                        std::cout << service_.authorityReview(
                                         session, std::stoll(id), decision == 1)
                                  << "\n";
                    }
                }
                pause();
            } else if (choice == 4) {
                settingsMenu(session);
            }
        } catch (const std::exception& error) {
            std::cout << red << error.what() << reset << "\n";
            pause();
        }
    }
}

void Ui::staffMenu(const Session& session) {
    for (;;) {
        printHeading("Staff dashboard");
        const int choice = choose(
            "Staff services",
            {"Info", "Transfer money", "Applications", "Loans and EMI", "Approvals",
             "User log", "Settings", "Cheque requests", "Loan disbursal", "Refresh",
             "Sign out"});
        if (choice == 0 || choice == 11) {
            return;
        }

        try {
            if (choice == 1) {
                showInfo(session);
            } else if (choice == 2) {
                transferMenu(session);
            } else if (choice == 3) {
                applyMenu(session);
            } else if (choice == 4) {
                loanMenu(session);
            } else if (choice == 5) {
                auto requests = service_.requestsFor(session);
                for (auto& row : requests) {
                    formatMoneyField(row, "amount");
                }
                printRows(requests, {"request_id", "request_type", "requester_name",
                                     "amount", "status", "created_at"});
                const auto id = ask("Request ID, or 0: ");
                if (id != "0" && !id.empty()) {
                    const int decision = choose("Decision",
                                                {"Approve / send onward", "Reject", "Back"});
                    if (decision < 3 && decision != 0) {
                        std::cout << service_.staffReview(
                                         session, std::stoll(id), decision == 1)
                                  << "\n";
                    }
                }
                pause();
            } else if (choice == 6) {
                const auto people = service_.bankInfo(
                    Session{0, "", "", Role::Authority});
                Rows users;
                for (const auto& person : people) {
                    if (valueOf(person, "role") == "USER") {
                        users.push_back(person);
                    }
                }
                printRows(users, {"visible_id", "name", "account_number", "login"});
                const auto id = ask("User s.no, or 0: ");
                if (id != "0" && !id.empty()) {
                    const auto visibleId = std::stoll(id);
                    for (auto row : service_.personInformation(
                             Session{0, "", "", Role::Authority}, "USER", visibleId)) {
                        formatMoneyField(row, "balance");
                        printVertical(row, {"name", "phone", "dob", "email", "address",
                                            "pan", "aadhaar", "login", "account_number",
                                            "credit_score", "balance"});
                    }
                    printRows(service_.personLog(session, "USER", visibleId),
                              {"created_at", "name", "account_number", "description"});
                }
                pause();
            } else if (choice == 7) {
                settingsMenu(session);
            } else if (choice == 8) {
                chequeMenu(session);
            } else if (choice == 9) {
                auto loans = service_.approvedLoans(session);
                for (auto& row : loans) {
                    formatMoneyField(row, "monthly_emi");
                    formatMoneyField(row, "outstanding");
                    formatMoneyField(row, "paid");
                }
                printRows(loans, {"s_no", "loan_id", "status", "monthly_emi",
                                  "next_due_date", "outstanding", "paid"});
                if (!loans.empty()) {
                    const auto id = ask("Approved loan s.no, or 0: ");
                    if (id != "0" && !id.empty()) {
                        const auto selected = std::stoll(id);
                        if (selected > 0 &&
                            selected <= static_cast<std::int64_t>(loans.size())) {
                            std::cout << service_.disburseLoan(
                                             session, numberOf(loans[static_cast<std::size_t>(selected - 1)],
                                                              "loan_id"))
                                      << "\n";
                        }
                    }
                }
                pause();
            }
        } catch (const std::exception& error) {
            std::cout << red << error.what() << reset << "\n";
            pause();
        }
    }
}

void Ui::userMenu(const Session& session) {
    for (;;) {
        printHeading("User dashboard");
        const int choice = choose(
            "Banking services",
            {"Info", "Transfer money", "Applications", "Loans and EMI",
             "Cheque requests", "Settings", "Account closure", "Transaction history",
             "Refresh", "Sign out"});
        if (choice == 0 || choice == 10) {
            return;
        }

        try {
            if (choice == 1) {
                showInfo(session);
            } else if (choice == 2) {
                transferMenu(session);
            } else if (choice == 3) {
                applyMenu(session);
            } else if (choice == 4) {
                loanMenu(session);
            } else if (choice == 5) {
                chequeMenu(session);
            } else if (choice == 6) {
                settingsMenu(session);
            } else if (choice == 7) {
                const auto accounts = service_.accountsForUser(session.userId);
                if (!accounts.empty()) {
                    std::cout << service_.requestClosure(
                                     session, valueOf(accounts.front(), "account_number"))
                              << "\n";
                }
                pause();
            } else if (choice == 8) {
                showHistory(session);
            }
        } catch (const std::exception& error) {
            std::cout << red << error.what() << reset << "\n";
            pause();
        }
    }
}

} // namespace jdn

