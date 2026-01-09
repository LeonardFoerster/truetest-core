# HFT-Engine (Work in Progress)

An Engine to backtest High-Frequenzy-Trading strategies. Using C++23 and PostgreSQL

## Build:
```
sudo apt update && sudo apt upgrade
sudo apt install git cmake build-essential postgresql postgresql-contrib libpq-dev
sudo systemctl start postgresql
sudo -u postgres createuser --interactive --pwprompt leonard  # Erstelle User 'leonard'
sudo -u postgres createdb storage  # Erstelle DB 'storage'
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
- Enhance Throughput to 1 Million order per second
- Monte Carlo Implementation


### Reference
- Simultanously I am building an UI: https://github.com/LeonardFoerster/TradeUI
