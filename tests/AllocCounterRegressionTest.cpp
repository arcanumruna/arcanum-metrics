#include "AllocCounter.h"

#include <gtest/gtest.h>

#include <string>

TEST(AllocCounterRegression, CountsOneAllocationForLongString) {
    volatile size_t n = 64;
    metrics::testing::AllocScope scope;
    {
        std::string s(n, 'x');
        EXPECT_EQ(s.size(), 64U);
    }
    EXPECT_EQ(scope.count(), 1);
}

TEST(AllocCounterRegression, CountsZeroWithoutAllocation) {
    metrics::testing::AllocScope scope;
    volatile int x = 0;
    x = x + 1;
    EXPECT_EQ(scope.count(), 0);
}
