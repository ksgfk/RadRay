#include <gtest/gtest.h>

// https://github.com/google/googletest/blob/main/googletest/src/gtest_main.cc

GTEST_API_ int main(int argc, char** argv) {
    printf("Running main() from %s\n", __FILE__);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
