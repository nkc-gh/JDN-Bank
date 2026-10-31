# JDN Bank

JDN Bank is a C++20 terminal-based banking management system backed by SQLite. It models a bank with separate Authority, Staff, and User roles, account management, financial transactions, approvals, loans, and cheque services.

## Features

- Authority, Staff, and User registration and login
- Role-based dashboards and permissions
- User profiles and KYC information
- Bank accounts with ledger-based balance calculation
- IMPS, NEFT, RTGS, and cheque transfers
- Staff approval for NEFT transfers
- Authority approval for RTGS transfers
- Loan requests, review, approval, disbursal, EMI schedules, and payments
- Cheque-book requests and issued-cheque tracking
- Transaction history, audit logs, and user activity logs
- Account settings and account closure requests
- SQLite transactions for safer financial operations

## Requirements

- C++20 compiler
- GNU Make
- SQLite3 development library

## How to run

### Linux

```bash
sudo apt update
sudo apt install g++ make libsqlite3-dev
make
./build/jdn-bank
```

### macOS

```bash
brew install gcc make sqlite3
make CXX=g++
./build/jdn-bank
```

### Windows with MSYS2

Install MSYS2, open the **MSYS2 UCRT64** terminal, and run:

```bash
pacman -Syu
pacman -S --needed mingw-w64-ucrt-x86_64-gcc make mingw-w64-ucrt-x86_64-sqlite3
cd "/c/path/to/JDN Bank"
make CXX=g++
./build/jdn-bank.exe
```

The current terminal interface uses POSIX headers, so MSYS2 UCRT64 or WSL2 is required on Windows. Native MSVC compilation is not supported.

### Clean rebuild

```bash
make clean
make
```

On the first run, the application creates `config/jdn_bank.conf` and the SQLite database at `data/jdn_bank.db`. The configuration file contains a generated authority registration key and should be kept private.

## Demo accounts

On a new database, the application creates demo accounts for testing:

| Role | Login | Password |
|---|---|---|
| Authority | `admin` | `admin123` |
| Staff | `staff` | `staff123` |
| User | `user1` | `user123` |
| User | `user2` | `user123` |

## Folder structure

```text
JDN Bank/
├── include/                    # Header files
│   ├── BankService.hpp         # Banking rules and workflows
│   ├── Database.hpp            # SQLite database interface
│   ├── Security.hpp            # Password and configuration support
│   ├── Types.hpp               # Shared types and helpers
│   └── Ui.hpp                  # Terminal user interface
├── src/                        # Source files
│   ├── main.cpp                # Application entry point
│   ├── BankService.cpp         # Banking logic
│   ├── Database.cpp            # Database schema and queries
│   ├── Security.cpp            # Password hashing and configuration
│   ├── Types.cpp               # Shared type implementations
│   └── Ui.cpp                  # Menus and terminal screens
├── Docs/                       # Project report and presentation files
├── Makefile                    # Build commands
├── LICENSE                     # MIT License for Team JDN
└── README.md                   # Project documentation
```

The `build/`, `config/`, and `data/` directories are generated locally when the project is built or run.

---

This project was developed for the **CSE 202: Object-Oriented Programming with C++** course.

Made by **Team JDN**.
