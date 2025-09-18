#pragma once 
#include <iostream>
#include <cmath> 

class black_scholes
{
public:
    
    black_scholes(double S, double K, double r, double sigma, double T, double d);
    double get_call_price() const;
    double get_put_price() const;
    double get_delta() const;
    double get_gamma() const;
    double get_theta() const;
    double get_vega() const;
    double get_rho() const;
    
private:

    double current_price = 0;
    double strike_price = 0;
    double riskfree_interest = 0;
    double standard_volatility = 0;
    double duration = 0;
    double dividend = 0;

  
    double cumulative_normal_distribution(double x) const;
    std::pair<double, double> calculate_d_values() const;

    

};