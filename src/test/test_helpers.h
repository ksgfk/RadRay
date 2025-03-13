#pragma once

#include <cstdlib>

#define RADRAY_TEST_TRUE(value) \
    do {                        \
        if (!(value)) {         \
            std::exit(-114514); \
        }                       \
    } while (0);

namespace radray {
};
