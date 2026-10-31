/**
 * @file Ui.hpp
 * @brief Terminal user interface for JDN Bank.
 */
#pragma once

#include "BankService.hpp"

namespace jdn {

/**
 * @brief Displays menus, reads input, and delegates work to BankService.
 *
 * Ui deliberately contains presentation code only. It should not decide
 * whether a transfer is legal; that decision belongs to BankService.
 */
class Ui {
public:
    explicit Ui(BankService& service);

    /** @brief Start the welcome, login, registration, and dashboard loop. */
    void run();

private:
    BankService& service_;

    /** @brief Top-level and authentication screens. */
    void welcome();
    void loginFlow(Role role);
    void registerFlow();

    /** @brief Role-specific dashboards. */
    void authorityMenu(const Session& session);
    void staffMenu(const Session& session);
    void userMenu(const Session& session);
    void authorityMenuV2(const Session& session);
    void staffMenuV2(const Session& session);
    void userMenuV2(const Session& session);

    /** @brief Reusable screens shared by dashboards. */
    void loanMenu(const Session& session);
    void chequeMenu(const Session& session);
    void showInfo(const Session& session, bool authorityView = false);
    void showHistory(const Session& session);
    void settingsMenu(const Session& session);
    void transferMenu(const Session& session);
    void applyMenu(const Session& session);

    /** @brief Small terminal input and output helpers. */
    void printRows(const Rows& rows, const std::vector<std::string>& columns) const;
    void printVertical(const Row& row, const std::vector<std::string>& fields) const;
    std::string ask(const std::string& label, bool required = true) const;
    std::string askSecret(const std::string& label) const;
    int choose(const std::string& title, const std::vector<std::string>& options) const;
    void pause() const;
};

} // namespace jdn

