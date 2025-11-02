#define _USE_MATH_DEFINES

#include "header/black_scholes.h"
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

    return current_price * exp(-dividend * duration) * nd1 - strike_price * exp(-riskfree_interest * duration) * nd2;
}

double black_scholes::get_put_price() const
{
    double d1 = calculate_d_values().first;
    double d2 = calculate_d_values().second;

    double n_d1 = cumulative_normal_distribution(-d1);
    double n_d2 = cumulative_normal_distribution(-d2);

    return strike_price * exp(-riskfree_interest * duration) * n_d2 - current_price * exp(-dividend * duration) * n_d1;
}

double black_scholes::cumulative_normal_distribution(double x) const
{
    return 0.5 * (1.0 + std::erf(x / sqrt(2.0)));
}

std::pair<double, double> black_scholes::calculate_d_values() const
{
    double log_cp_sp = log(current_price / strike_price);
    double temp = riskfree_interest - dividend + (pow(standard_volatility, 2) / 2.0);
    double d1_numerator = log_cp_sp + (temp * duration);
    double d1_denominator = standard_volatility * sqrt(duration);

    double d1 = d1_numerator / d1_denominator;
    double d2 = d1 - standard_volatility * sqrt(duration);

    return { d1, d2 };
}

double black_scholes::normal_density(double x) const
{
    double first_factor = 1.0 / sqrt(2.0 * M_PI);
    double second_factor = exp(-pow(x, 2) / 2.0);
    return first_factor * second_factor;
}

double black_scholes::get_delta() const
{
    double d1 = calculate_d_values().first;
    double nd1 = cumulative_normal_distribution(d1);
    return exp(-dividend * duration) * nd1;
}

double black_scholes::get_gamma() const
{
    double d1 = calculate_d_values().first;
    double dividend_val = normal_density(d1) * exp(-dividend * duration);
    double divisor = current_price * standard_volatility * sqrt(duration);
    return dividend_val / divisor;
}

double black_scholes::get_theta() const
{
    double d1 = calculate_d_values().first;
    double d2 = calculate_d_values().second;

    double first_term = -(current_price * normal_density(d1) * standard_volatility * exp(-dividend * duration)) / (2.0 * sqrt(duration));
    double second_term = riskfree_interest * strike_price * exp(-riskfree_interest * duration) * cumulative_normal_distribution(d2);
    double third_term = dividend * current_price * exp(-dividend * duration) * cumulative_normal_distribution(d1);

    double theta_per_year = first_term - second_term + third_term;
    return theta_per_year / days_in_year;
}

double black_scholes::get_vega() const
{
    double d1 = calculate_d_values().first;
    double vega_value = current_price * sqrt(duration) * normal_density(d1) * exp(-dividend * duration);
    return vega_value / 100.0;
}

double black_scholes::get_call_rho() const
{
    double d2 = calculate_d_values().second;
    double nd2 = cumulative_normal_distribution(d2);
    double rho_value = strike_price * duration * exp(-riskfree_interest * duration) * nd2;
    return rho_value / 100.0;
}

double black_scholes::get_put_rho() const
{
    double d2 = calculate_d_values().second;
    double n_d2 = cumulative_normal_distribution(-d2);
    double rho_value = -strike_price * duration * exp(-riskfree_interest * duration) * n_d2;
    return rho_value / 100.0;
}






