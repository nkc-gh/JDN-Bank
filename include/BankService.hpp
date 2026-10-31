/**
 * @file BankService.hpp
 * @brief Business rules for JDN Bank.
 */
#pragma once

#include "Database.hpp"
#include "Security.hpp"
#include "Types.hpp"
#include <optional>

namespace jdn {

/**
 * @brief Application service containing banking rules and authorization.
 *
 * Ui calls this class, and this class calls Database. Keeping those jobs
 * separate is the project's main example of encapsulation and layering:
 * the UI knows how to ask questions, while BankService decides whether an
 * operation is allowed and what records must be written.
 */
class BankService {
public:
    BankService(Database& database, Security& security);

    /** @brief Prepare the schema, system accounts, and first-run data. */
    void initialise();

    /** @brief Authenticate a user and return a Session on success. */
    std::optional<Session> login(Role role, const std::string& login,
                                 const std::string& password,
                                 std::string& message);

    /** @brief Create an authority account after checking the secret key. */
    std::string registerAuthority(const std::string& name, const std::string& login,
                                  const std::string& password, const std::string& email,
                                  const std::string& phone, const std::string& pan,
                                  const std::string& aadhaar, const std::string& secret);
    /** @brief Create a pending staff registration request. */
    std::string requestStaffRegistration(const std::string& name, const std::string& login,
                                         const std::string& password, const std::string& email,
                                         const std::string& phone, const std::string& pan,
                                         const std::string& aadhaar);
    /** @brief Create a pending user registration request. */
    std::string requestUserRegistration(const std::string& name, const std::string& login,
                                        const std::string& password, const std::string& email,
                                        const std::string& phone, const std::string& aadhaar,
                                        const std::string& pan, Money openingBalance,
                                        std::optional<std::int64_t> staffId);

    /** @brief Registration entry points that include full profile details. */
    std::string registerAuthorityFull(const std::string& name, const std::string& mobile,
                                      const std::string& dob, const std::string& email,
                                      const std::string& address, const std::string& pan,
                                      const std::string& aadhaar, const std::string& login,
                                      const std::string& password, const std::string& secret);
    std::string requestStaffRegistrationFull(const std::string& name, const std::string& mobile,
                                             const std::string& dob, const std::string& email,
                                             const std::string& address, const std::string& pan,
                                             const std::string& aadhaar, const std::string& login,
                                             const std::string& password);
    std::string requestUserRegistrationFull(const std::string& name, const std::string& mobile,
                                            const std::string& dob, const std::string& email,
                                            const std::string& address, const std::string& pan,
                                            const std::string& aadhaar, const std::string& login,
                                            const std::string& password);

    /** @brief Approval workflow methods. */
    Rows requestsFor(const Session& session) const;
    std::string staffReview(const Session& staff, std::int64_t requestId, bool approve);
    std::string authorityReview(const Session& authority, std::int64_t requestId, bool approve);

    /** @brief Information and reporting queries. */
    Rows staffDirectory() const;
    Rows accountsForUser(std::int64_t userId, bool includeClosed = false) const;
    Rows allAccounts() const;
    Rows informationForAuthority(const Session& authority, const std::string& role) const;
    Rows informationForStaff(const Session& staff) const;
    Rows ownInformation(const Session& user) const;
    Rows personLog(const Session& viewer, const std::string& role, std::int64_t visibleId) const;
    Rows activityLog(const Session& authority, const std::string& role) const;
    Rows bankInfo(const Session& authority) const;
    Rows personInformation(const Session& authority, const std::string& role,
                           std::int64_t visibleId) const;
    Rows bankSummary(const Session& authority) const;

    /** @brief Loan and cheque reporting queries. */
    Rows loanDetails(const Session& viewer, std::optional<std::int64_t> userId = std::nullopt) const;
    Rows emiSchedule(const Session& viewer) const;
    Rows approvedLoans(const Session& staff) const;
    Rows chequeRequests(const Session& viewer) const;
    Rows chequesFor(const Session& viewer) const;

    /** @brief Account balance and transfer operations. */
    Money balance(std::int64_t accountId);
    void accrueInterest(std::int64_t accountId);
    std::string transfer(const Session& actor, const std::string& fromNumber,
                         const std::string& toNumber, Money amount,
                         const std::string& rail, const std::string& memo);
    std::string transferByCheque(const Session& actor, const std::string& fromNumber,
                                 const std::string& toNumber, const std::string& chequeNumber,
                                 Money amount, const std::string& recipientName);
    std::string processNeft(const Session& staff);

    /** @brief Loan, deposit, closure, and cheque actions. */
    std::string requestLoan(const Session& user, Money amount, const std::string& purpose,
                            int termMonths);
    std::string payEmi(const Session& payer, std::int64_t loanId, Money amount);
    std::string requestAddMoney(const Session& user, const std::string& accountNumber, Money amount);
    std::string approveAddMoney(const Session& staff, std::int64_t requestId, bool approve);
    std::string disburseLoan(const Session& staff, std::int64_t loanId);
    std::string requestClosure(const Session& user, const std::string& accountNumber);
    std::string setAccountFrozen(const Session& authority, const std::string& accountNumber,
                                 bool frozen);
    std::string requestChequeBook(const Session& user, const std::string& accountNumber,
                                  int leaves = 10,
                                  const std::string& address = "Registered address");
    std::string updateCheque(const Session& staff, const std::string& chequeNumber,
                             const std::string& state);

    /** @brief History, audit, fraud, and export operations. */
    Rows history(std::int64_t accountId) const;
    Rows auditLog(int limit = 100) const;
    Rows fraudFlags() const;
    std::string exportHistory(std::int64_t accountId, const std::string& path) const;

    /** @brief Update profile data or one setting after authorization checks. */
    std::string updateProfile(const Session& actor, const std::string& name,
                              const std::string& email, const std::string& phone,
                              const std::string& pan, const std::string& aadhaar,
                              const std::string& newPassword,
                              const std::string& authorityKey);
    std::string updateSetting(const Session& actor, const std::string& field,
                              const std::string& value, const std::string& authorityKey);

private:
    Database& db_;
    Security& security_;

    /** @brief Internal lookup and validation helpers. */
    std::optional<Row> userByLogin(const std::string& login) const;
    std::optional<Row> accountByNumber(const std::string& number) const;
    std::string makeAccountNumber(std::int64_t userId, const std::string& role) const;
    bool validAccountNumber(const std::string& value) const;

    /** @brief Record activity and post balanced ledger transactions. */
    void audit(const std::optional<Session>& actor, const std::string& event,
               const std::string& details) const;
    void postTransfer(std::int64_t fromAccount, std::int64_t toAccount, Money amount,
                      const std::string& type, const std::string& memo,
                      const std::optional<Session>& actor);
    void flagFraud(std::int64_t transactionId, std::int64_t recipientAccount,
                   Money amount, const std::optional<Session>& actor);
    std::int64_t systemAccount(const std::string& number) const;

    /** @brief Turn approved registration requests into real accounts. */
    void createApprovedUser(const Row& request, std::int64_t authorityId);
    void createApprovedStaff(const Row& request, std::int64_t authorityId);

    /** @brief Populate a new database with clearly labelled demo data. */
    void seedDemoData();
};

} // namespace jdn

