#include <iostream>
#include <cmath>
#include <vector>
#include "black_scholes.h"
#include "black_scholes.cpp"
#include <cmath>

double black_scholes::get_delta() const
{
	double d1 = calculate_d_values().first;
	double nd1 = cumulative_normal_distribution(d1);

	double e = pow(exp(1), -dividend * duration);

	return e * nd1;
}

void test()

}