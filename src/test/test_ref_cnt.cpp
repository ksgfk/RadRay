#include <thread>
#include <vector>
#include <chrono>

#include <radray/logger.h>
#include <radray/core/object.h>

using radray::RC;

class Test : public radray::Object {
public:
    Test(int32_t value) noexcept : radray::Object(), _value{value} {
        RADRAY_INFO_LOG("ctor {}", _value);
    }
    ~Test() noexcept override {
        RADRAY_INFO_LOG("dtor {}", _value);
    }

private:
    int32_t _value;
};

class Child : public Test {
public:
    Child(int32_t value, int32_t child) : Test(value), _child(child) {
        RADRAY_INFO_LOG("ctor Child {}", child);
    }
    ~Child() noexcept override {
        RADRAY_INFO_LOG("dtor Child {}", _child);
    }

private:
    int32_t _child;
};

void Simple() {
    {
        RC<Test> a = radray::MakeObject<Test>(1);
    }
    {
        RC<Test> a = radray::MakeObject<Test>(2);
        RC<Test> b{a};
    }
    {
        RC<Test> a = radray::MakeObject<Test>(3);
        RC<Test> b = a;
    }
    {
        RC<Test> a = radray::MakeObject<Test>(4);
        RC<Test> b{std::move(a)};
    }
    {
        RC<Test> a = radray::MakeObject<Test>(5);
        RC<Test> b = std::move(a);
    }
    {
        RC<Test> a = radray::MakeObject<Child>(6, 6);
    }
    {
        RC<Test> b{radray::MakeObject<Child>(7, 7)};
    }
}

void MT() {
    std::vector<std::thread> ts;
    {
        RC<Test> c = radray::MakeObject<Child>(1, 1);
        for (int i = 0; i < 10; i++) {
            std::thread thread{[c, i]() {
                RC<Test> cp = c;
                RADRAY_INFO_LOG("thread run {}", i);
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                RADRAY_INFO_LOG("thread stop {}", i);
            }};
            ts.emplace_back(std::move(thread));
        }
    }
    for (auto&& i : ts) {
        i.join();
    }
}

int main() {
    Simple();
    RADRAY_INFO_LOG("------------------------");
    MT();
    return 0;
}
