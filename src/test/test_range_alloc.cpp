#include <algorithm>
#include <radray/types.h>

using namespace radray;
using UINT = radray::uint32_t;

#define RADRAY_TEST_TRUE(value) \
    do {                        \
        if (!(value)) {         \
            std::abort();       \
        }                       \
    } while (0);

class Test {
public:
    vector<uint32_t> _empty{};
    uint32_t _allocIndex{0};

public:
    uint32_t AllocateRange(uint32_t count) noexcept {
        std::sort(_empty.begin(), _empty.end());
        UINT continuous = 0;
        size_t i = 0;
        for (; i < _empty.size(); i++) {
            UINT v = _empty[i];
            continuous++;
            if (i != 0) {
                UINT last = _empty[i - 1];
                if (last != v - 1) {
                    continuous = 1;
                }
            }
            if (continuous >= count) {
                break;
            }
        }
        if (continuous >= count) {
            size_t start = i - count + 1;
            size_t end = i + 1;
            UINT result = _empty[start];
            _empty.erase(_empty.begin() + start, _empty.begin() + end);
            return result;
        }
        UINT v = 0, start = _allocIndex;
        if (!_empty.empty() && _empty[_empty.size() - 1] == (_allocIndex - 1) && continuous > 0) {
            v = continuous;
            start = _allocIndex - v;
            auto begin = std::lower_bound(_empty.begin(), _empty.end(), start);
            _empty.erase(begin, _empty.end());
        }
        _allocIndex += count - v;
        return start;
    }
};

void T1() {
    Test a{};
    auto v1 = a.AllocateRange(5);
    RADRAY_TEST_TRUE(v1 == 0);
    RADRAY_TEST_TRUE(a._allocIndex == 5);
}

void T2() {
    /**
     * x = 被使用, o = 空
     * start                   p    end
     * |xxoxo oxoxx oooox xoooo|ooooooo|
     */
    Test a{};
    a._empty = {2, 4, 5, 7, 10, 11, 12, 13, 16, 17, 18, 19};
    a._allocIndex = 20;
    auto v1 = a.AllocateRange(2);
    RADRAY_TEST_TRUE(v1 == 4);
    RADRAY_TEST_TRUE(a._allocIndex == 20);
    RADRAY_TEST_TRUE(a._empty.size() == 10);
    RADRAY_TEST_TRUE(a._empty[0] == 2);
    RADRAY_TEST_TRUE(a._empty[1] == 7);
}

void T3() {
    /**
     * x = 被使用, o = 空
     * start                   p    end
     * |ooxxx oxoxx oooox xoooo|ooooooo|
     */
    Test a{};
    a._empty = {0, 1, 5, 7, 10, 11, 12, 13, 16, 17, 18, 19};
    a._allocIndex = 20;
    auto v1 = a.AllocateRange(2);
    RADRAY_TEST_TRUE(v1 == 0);
    RADRAY_TEST_TRUE(a._allocIndex == 20);
    RADRAY_TEST_TRUE(a._empty.size() == 10);
    RADRAY_TEST_TRUE(a._empty[0] == 5);
    RADRAY_TEST_TRUE(a._empty[1] == 7);
}

void T4() {
    /**
     * x = 被使用, o = 空
     * start       p       end
     * |xxxxx xxxoo|ooooooo|
     */
    Test a{};
    a._empty = {8, 9};
    a._allocIndex = 10;
    auto v1 = a.AllocateRange(2);
    RADRAY_TEST_TRUE(v1 == 8);
    RADRAY_TEST_TRUE(a._allocIndex == 10);
    RADRAY_TEST_TRUE(a._empty.size() == 0);
}

void T5() {
    /**
     * x = 被使用, o = 空
     * start                   p    end
     * |xxoxo oxoxx oooox xoooo|ooooooo|
     */
    Test a{};
    a._empty = {2, 4, 5, 7, 10, 11, 12, 13, 16, 17, 18, 19};
    a._allocIndex = 20;
    auto v1 = a.AllocateRange(5);
    RADRAY_TEST_TRUE(v1 == 16);
    RADRAY_TEST_TRUE(a._allocIndex == 21);
    RADRAY_TEST_TRUE(a._empty.size() == 8);
}

int main() {
    T1();
    T2();
    T3();
    T4();
    T5();
    return 0;
}