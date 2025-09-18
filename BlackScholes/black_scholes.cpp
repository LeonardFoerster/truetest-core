#define _USE_MATH_DEFINES

#include "black_scholes.h" 
#include <iostream>        
#include <cmath>
#include <utility>

black_scholes::black_scholes(double S, double K, double r, double sigma, double T, double d)
    : current_price(S), strike_price(K), riskfree_interest(r), standard_volatility(sigma), duration(T), dividend(d)
{
}

double black_scholes::get_call_price() const
{
    double d1 = calculate_d_values().first;
    double d2 = calculate_d_values().second;
    
    double nd1 = cumulative_normal_distribution(d1);
    double nd2 = cumulative_normal_distribution(d2);

    return current_price * nd1 - strike_price * exp(-riskfree_interest * duration) * nd2;
}

double black_scholes::get_put_price() const
{
    double d1 = calculate_d_values().first;
    double d2 = calculate_d_values().second;

    double nd1 = cumulative_normal_distribution(-d1);
    double nd2 = cumulative_normal_distribution(-d2);

    return strike_price * pow(M_E, -riskfree_interest * duration) * nd2 - current_price * nd1;
}

double black_scholes::cumulative_normal_distribution(double x) const
{
    return 0.5 * (1.0 + std::erf(x / sqrt(2.0)));
}


std::pair<double, double> black_scholes::calculate_d_values() const
{
    //Calc for d1
    double log_cp_sp = log(current_price / strike_price);
    double temp = riskfree_interest + (pow(standard_volatility, 2) / 2.0);
    double dividend = log_cp_sp + (temp * duration);
    double divisor = standard_volatility * sqrt(duration);

    double d1 = dividend / divisor;
    double d2 = d1 - standard_volatility * sqrt(duration);

    return { d1, d2 };
}