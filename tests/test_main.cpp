#include <gtest/gtest.h>
#include "helpers/test_ui.h"

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    install_truetest_listener();
    return RUN_ALL_TESTS();
}
