#define _USE_MATH_DEFINES

#include <iostream>
#include <cmath>
#include <vector>
#include "black_scholes.h"




double black_scholes::get_delta() const
{
	double d1 = calculate_d_values().first;
	double nd1 = cumulative_normal_distribution(d1);

	double e = pow(exp(1), -dividend * duration);

	return e * nd1;
}


double  black_scholes::get_gamma() const
{
	double d1 = calculate_d_values().first;

	double dividend_first_factor = 1 / sqrt(2 * M_PI);
	double dividend_second_factor = pow(M_E, -pow(d1, 2) / 2);

	double dividend = dividend_first_factor * dividend_second_factor;
	double divisor = current_price * standard_volatility * sqrt(duration);

	return dividend / divisor;
}


double black_scholes::get_theta() const
{

	//thfasdfadhfh

}

double black_scholes::get_vega() const
{

}

double black_scholes::get_rho() const
{

}






