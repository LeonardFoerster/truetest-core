# HFT-Engine (Work in Progress)
**Currently there is an issue regarding CMake. I'll fix this asap!**  

An Engine to backtest High-Frequenzy-Trading strategies. Using C++23 and PostgreSQL


**I am working on a solution to import your own data via CSV.  Currently, only the following format works:**  
```
ID, symbol, date, time (without zone), open, high, low, close, volume
```


## Build:
```
sudo apt update && sudo apt upgrade
sudo apt install git cmake build-essential postgresql postgresql-contrib libpq-dev
sudo systemctl start postgresql
sudo -u postgres createuser --interactive --pwprompt "your-username"  # Create your user
sudo -u postgres createdb "your-database"  # Create your database
git clone https://github.com/LeonardFoerster/hft-engine.git
cd hft-engine
mkdir build && cd build
cmake ..
cmake --build .
./hft-engine2

(Windows is following)
```




### To-Do
- Multi Order Matching Logic
- Monte Carlo Implementation


### Reference
- Simultanously I am building an UI: https://github.com/LeonardFoerster/TradeUI
