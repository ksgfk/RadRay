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

    uint64_t Release() override {
        if (_refCount == 0) {
            RADRAY_ABORT("repeat release");
        }
        RADRAY_INFO_LOG("release {}", _value);
        _refCount--;
        if (_refCount == 0) {
            delete this;
        }
        return _refCount;
    }

private:
    uint64_t _refCount{1};
    int32_t _value;
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
    return 0;
}
