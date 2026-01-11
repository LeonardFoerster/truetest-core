# HFT-Engine (Work in Progress)

An Engine to backtest High-Frequenzy-Trading strategies. Using C++23 and PostgreSQL


**I am working on a solution to import your own data via CSV.  Currently, only the following format works:**  
```
ID, symbol, date, time (without zone), open, high, low, close, volume
```


## Build Instructions

This project now automatically downloads and builds `libpqxx` from source. The only manual dependency you need to install is PostgreSQL.

### 1. Install PostgreSQL

You must have PostgreSQL installed and available in your system's PATH. You can download it from the official website: [https://www.postgresql.org/download/](https://www.postgresql.org/download/)

Make sure to install the development headers for PostgreSQL. If you use the graphical installer, they are usually included.

Additionally, you need to install the PostgreSQL client library (`libpq`) for your operating system:

*   **Ubuntu/Debian:** `sudo apt-get install libpq-dev`
*   **macOS:** `brew install postgresql` (installs both server and client libraries)
*   **Windows (using vcpkg):** `vcpkg install libpq` (you might need to install `vcpkg` first if you don't have it)

### 2. Configure the project with CMake

Once PostgreSQL is installed, you can configure the project.

```bash
# It is recommended to start with a clean build directory
cd hft-engine
rm -rf build
mkdir build
cd build

# Configure the project
cmake ..
```

When you run `cmake`, it will first try to find your PostgreSQL installation. Then, it will download and compile `libpqxx`. This might take a few minutes the first time.

### 3. Build the project

Once CMake has been configured successfully, you can build the project with:

```bash
cmake --build .
```

This will create the `hft-engine2` executable in the `build` directory.



### Additionally
- Simultanously I am building an UI: https://github.com/LeonardFoerster/TradeUI
