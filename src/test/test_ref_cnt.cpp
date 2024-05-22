#include <radray/logger.h>
#include <radray/core/object.h>

class Test : public radray::Object {
public:
    Test(int32_t value) noexcept : _value{value} {
        RADRAY_INFO_LOG("ctor {}", _value);
    }
    ~Test() noexcept override {
        RADRAY_INFO_LOG("dtor {}", _value);
    }

    uint64_t AddRef() override {
        RADRAY_INFO_LOG("add ref {}", _value);
        _refCount++;
        return _refCount;
    }

    uint64_t RemoveRef() override {
        RADRAY_INFO_LOG("release {}", _value);
        _refCount--;
        return _refCount;
    }

private:
    uint64_t _refCount{0};
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

int main() {
    using radray::RC;
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
    return 0;
}
