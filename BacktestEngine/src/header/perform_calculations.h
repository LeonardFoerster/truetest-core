#pragma once

#include "black_scholes.h"

#ifndef PERFORM_CALCULATIONS_H
#define PERFORM_CALCULATIONS_H

void run_manual_calc();
int run_csv_calc();
void print_manual_results(const black_scholes& option_data);

#endif