#pragma once

#include <gtest/gtest.h>
#include <iostream>
#include <string>
#include <chrono>
#include <iomanip>

class TrueTestListener : public ::testing::EmptyTestEventListener
{
    int passed_  = 0;
    int failed_  = 0;
    int skipped_ = 0;
    int total_ = 0;
    std::chrono::steady_clock::time_point suite_start_;
    std::chrono::steady_clock::time_point test_start_;

    static std::string pad(const std::string& s, int width)
    {
        if (static_cast<int>(s.size()) >= width) return s;
        return s + std::string(width - s.size(), ' ');
    }

    void print_bar(int pass, int fail, int total) const
    {
        const int bar_width = 40;
        int filled = (total > 0) ? (pass * bar_width / total) : 0;
        int fail_fill = (total > 0) ? (fail * bar_width / total) : 0;
        int empty = bar_width - filled - fail_fill;

        std::cout << "  [";
        for (int i = 0; i < filled; ++i)   std::cout << "#";
        for (int i = 0; i < fail_fill; ++i) std::cout << "!";
        for (int i = 0; i < empty; ++i)    std::cout << ".";
        std::cout << "] " << pass + fail << "/" << total << "\n";
    }

public:
    void OnTestProgramStart(const ::testing::UnitTest& unit_test) override
    {
        total_ = unit_test.total_test_count();
        suite_start_ = std::chrono::steady_clock::now();

        std::cout << "\n";
        std::cout << "   _____                _____         _   \n";
        std::cout << "  |_   _| __ _   _  ___|_   _|__  ___| |_ \n";
        std::cout << "    | || '__| | | |/ _ \\ | |/ _ \\/ __| __|\n";
        std::cout << "    | || |  | |_| |  __/ | |  __/\\__ \\ |_ \n";
        std::cout << "    |_||_|   \\__,_|\\___| |_|\\___||___/\\__|\n";
        std::cout << "\n";
        std::cout << "    Unit Test Suite\n";
        std::cout << "\n";
        std::cout << "  +-------------------------------------------+\n";
        std::cout << "  |  Running " << std::setw(4) << total_ << " tests across "
                  << std::setw(2) << unit_test.total_test_suite_count()
                  << " suites        |\n";
        std::cout << "  +-------------------------------------------+\n\n";
    }

    void OnTestSuiteStart(const ::testing::TestSuite& suite) override
    {
        std::cout << "  --- " << suite.name()
                  << " (" << suite.total_test_count() << " tests) ";
        int dashes = 38 - static_cast<int>(std::string(suite.name()).size());
        for (int i = 0; i < dashes; ++i) std::cout << "-";
        std::cout << "\n";
    }

    void OnTestStart(const ::testing::TestInfo& /*test_info*/) override
    {
        test_start_ = std::chrono::steady_clock::now();
    }

    void OnTestEnd(const ::testing::TestInfo& test_info) override
    {
        auto elapsed = std::chrono::steady_clock::now() - test_start_;
        auto ms = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

        // gtest's Passed() returns false for both real failures AND
        // GTEST_SKIP, so distinguish them: skipped tests count as
        // neither passed nor failed (they're conditional opt-outs,
        // typically guarded on env vars like QUESTDB_TEST_HOST).
        const auto* result = test_info.result();
        const bool skipped = result->Skipped();
        const bool ok      = result->Passed() || skipped;
        if (skipped)      ++skipped_;
        else if (ok)      ++passed_;
        else              ++failed_;

        const char* status = skipped ? "SKIP" : (ok ? " ok " : "FAIL");
        std::string name = pad(test_info.name(), 42);

        std::cout << "    " << status << "  " << name;
        if (ms > 1000)
            std::cout << std::fixed << std::setprecision(1) << (ms / 1000.0) << " ms";
        std::cout << "\n";

        if (!ok)
        {
            // Print failure details indented
            for (int i = 0; i < result->total_part_count(); ++i)
            {
                const auto& part = result->GetTestPartResult(i);
                if (part.failed())
                {
                    std::cout << "          ^ " << part.file_name() << ":"
                              << part.line_number() << "\n";
                    std::cout << "            " << part.summary() << "\n";
                }
            }
        }
    }

    void OnTestSuiteEnd(const ::testing::TestSuite& /*suite*/) override
    {
        std::cout << "\n";
    }

    void OnTestProgramEnd(const ::testing::UnitTest& /*unit_test*/) override
    {
        auto elapsed = std::chrono::steady_clock::now() - suite_start_;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

        std::cout << "  +-------------------------------------------+\n";

        print_bar(passed_, failed_, total_);
        std::cout << "\n";

        if (failed_ == 0)
        {
            std::cout << "       ______________________\n";
            std::cout << "      /                      \\\n";
            std::cout << "     |   ALL " << std::setw(4) << passed_ << " TESTS PASSED";
            if (skipped_ > 0)
                std::cout << " (" << skipped_ << " skipped)";
            std::cout << "   |\n";
            std::cout << "      \\______________________/\n";
            std::cout << "             \\   ^__^\n";
            std::cout << "              \\  (oo)\\_______\n";
            std::cout << "                 (__)\\       )\\/\\\n";
            std::cout << "                     ||----w |\n";
            std::cout << "                     ||     ||\n";
        }
        else
        {
            std::cout << "      +--------------------------+\n";
            std::cout << "      |  " << std::setw(4) << passed_ << " passed  "
                      << std::setw(4) << failed_ << " failed    |\n";
            std::cout << "      +--------------------------+\n";
            std::cout << "             \\   ^__^\n";
            std::cout << "              \\  (xx)\\_______\n";
            std::cout << "                 (__)\\       )\\/\\\n";
            std::cout << "                  U  ||----w |\n";
            std::cout << "                     ||     ||\n";
        }

        std::cout << "\n  Completed in " << ms << " ms\n\n";
    }
};

// Call this from main() BEFORE RUN_ALL_TESTS
inline void install_truetest_listener()
{
    auto& listeners = ::testing::UnitTest::GetInstance()->listeners();
    // Remove default printer
    delete listeners.Release(listeners.default_result_printer());
    // Install custom listener
    listeners.Append(new TrueTestListener());
}
