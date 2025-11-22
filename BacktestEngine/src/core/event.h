#pragma once
#include <iostream>
#include <chrono>
#include <string>
#include <memory>

enum class event_type
{
	market,
	signal,
	order,
	fill
};


class event 
{
public:
	event(event_type type, std::chrono::system_clock::time_point timestamp)
		: type_(type)
		, timestamp_ (timestamp)
	{ }

	virtual ~event() = default;

	event_type get_type() const { return type_; }
	std::chrono::system_clock::time_point get_timestamp() const { return timestamp_; }

	virtual std::string to_string() const { return "Event[type=" + std::to_string(static_cast<int>(type_)) + "]"; }

protected:
	event_type type_;
	std::chrono::system_clock::time_point timestamp_;
};


using event_pointer = std::shared_ptr<event>;


class market_event : public event
{
public:
	market_event
	(
		std::chrono::system_clock::time_point timestamp,
		const std::string& symbol,
		double open,
		double high,
		double low,
		double close,
		int64_t volume = 0
	)

		:event(event_type::market, timestamp)
		, symbol_ (symbol)
		, open_ (open)
		, high_ (high)
		, close_ (close)
		, volume_ (volume)
	{ }

	const std::string& get_symbol() const { return symbol_; }
	double get_open() const { return open_; }
	double get_high() const { return high_; }
	double get_low() const { return low_; }
	double get_close() const { return close_; }
	int64_t get_volume() const { return volume_; }

	std::string to_string() const override 
	{
		return "MarketEvent[" + symbol_ + " OHLC=" +
			std::to_string(open_) + "/" + std::to_string(high_) + "/" +
			std::to_string(low_) + "/" + std::to_string(close_) + "]";
	}

private:
	std::string symbol_;
	double open_;
	double high_;
	double low_;
	double close_; 
	int64_t volume_;

};

enum class signal_type
{
	buy,
	sell,
	hold
};


class signal_event : public event
{
public:
	signal_event
	(
		std::chrono::system_clock::time_point timestamp,
		const std::string& symbol,
		signal_type signal,
		double strength = 1.0
	)
	: event(event_type::signal, timestamp)
	, symbol_(symbol)
	, signal_(signal)
	, strength_(strength)
	{ }

	const std::string& get_symbol() const { return symbol_; }
	signal_type get_signal() const { return signal_; }
	double get_strength() const { return strength_; }
	
	std::string to_string() const override 
	{
		std::string signal_str;
		switch (signal_) 
		{
			case signal_type::buy: signal_str = "BUY"; break;
			case signal_type::sell: signal_str = "SELL"; break;
			case signal_type::hold: signal_str = "HOLD"; break;
		}
			return "SignalEvent[" + symbol_ + " " + signal_str + " strength=" + std::to_string(strength_) + "]";
	}

private:
	std::string symbol_;
	signal_type signal_;
	double strength_ = 0;
};


enum class order_type
{
	market,
	limit,
	stop
};

enum class order_side
{
	buy,
	sell
};

class order_event : public event 
{
public:
	order_event
	(
		std::chrono::system_clock::time_point timestamp,
		const std::string& symbol,
		order_type order_type,
		order_side side,
		int quantity,
		double price = 0.0
	)
		: event(event_type::order, timestamp)
		, symbol_(symbol)
		, order_type_(order_type)
		, side_(side)
		, quantity_(quantity)
		, price_(price)
	{
	}

	const std::string& get_symbol() const { return symbol_; }
	order_type get_order_type() const { return order_type_; }
	order_side get_side() const { return side_; }
	int get_quantity() const { return quantity_; }
	double get_price() const { return price_; }

	std::string to_string() const override 
	{
		std::string side_str = (side_ == order_side::buy) ? "BUY" : "SELL";
		std::string type_str;
		switch (order_type_) {
		case order_type::market: type_str = "MARKET"; break;
		case order_type::limit: type_str = "LIMIT"; break;
		case order_type::stop: type_str = "STOP"; break;
		}
		return "OrderEvent[" + type_str + " " + side_str + " " +
			std::to_string(quantity_) + " " + symbol_ +
			" @ " + std::to_string(price_) + "]";
	}

private:
	std::string symbol_;
	order_type order_type_;
	order_side side_;
	int quantity_;
	double price_;
};

class fill_event : public event 
{
public:
	fill_event
	(
		std::chrono::system_clock::time_point timestamp,
		const std::string& symbol,
		order_side side,
		int filled_quantity,
		double fill_price,
		double commission = 0.0
	)
		: event(event_type::fill, timestamp)
		, symbol_(symbol)
		, side_(side)
		, filled_quantity_(filled_quantity)
		, fill_price_(fill_price)
		, commission_(commission)
	{
	}

	const std::string& get_symbol() const { return symbol_; }
	order_side get_side() const { return side_; }
	int get_filled_quantity() const { return filled_quantity_; }
	double get_fill_price() const { return fill_price_; }
	double get_commission() const { return commission_; }

	
	double get_total_cost() const 
	{
		double base = filled_quantity_ * fill_price_;
		return (side_ == order_side::buy) ? base + commission_ : base - commission_;
	}

	std::string to_string() const override 
	{
		std::string side_str = (side_ == order_side::buy) ? "BOUGHT" : "SOLD";
		return "FillEvent[" + side_str + " " +
			std::to_string(filled_quantity_) + " " + symbol_ +
			" @ " + std::to_string(fill_price_) +
			" commission=" + std::to_string(commission_) + "]";
	}

private:
	std::string symbol_;
	order_side side_;
	int filled_quantity_;
	double fill_price_;
	double commission_;
};