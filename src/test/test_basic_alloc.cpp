#include <cstdio>
#include <stdexcept>

#include <radray/types.h>

class A {
public:
    explicit A(int c) : _c(c) {
        printf("1 ctor\n");
        throw std::runtime_error{"err"};
    }
    ~A() noexcept {
        printf("2 dtor\n");
    }

    int C() noexcept { return _c; }

private:
    int _c;
};

int main() {
    try {
        A* a = new A{1};
        printf("3 %d\n", a->C());
    } catch (const std::exception& e) {
        printf("4 %s\n", e.what());
    }
    return 0;
}
