/**
 * @file main.cpp
 * @brief Application entry point and object composition root.
 */
#include "BankService.hpp"
#include "Database.hpp"
#include "Security.hpp"
#include "Ui.hpp"

#include <exception>
#include <iostream>

/**
 * @brief Create the application's objects and start the terminal UI.
 *
 * main() is intentionally small. It demonstrates composition: concrete
 * objects are created here and references are passed to the classes that need
 * them. The UI does not create its own database connection.
 */
int main() {
    try {
        // Config owns paths and secrets loaded from the local config file.
        const auto config = jdn::Config::loadOrCreate("config/jdn_bank.conf");

        // These objects live until main() ends. Their destruction order safely
        // closes the UI, service references, security object, and database.
        jdn::Database database(config.databasePath);
        jdn::Security security(config);
        jdn::BankService bank(database, security);
        bank.initialise();

        jdn::Ui ui(bank);
        ui.run();
        return 0;
    } catch (const std::exception& error) {
        // A startup failure is reported once instead of showing a half-working
        // banking screen with an unknown database state.
        std::cerr << "\nJDN Bank could not start safely: " << error.what() << "\n";
        return 1;
    }
}

