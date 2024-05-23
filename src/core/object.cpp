#include <radray/core/object.h>

namespace radray {

uint64_t Object::AddRef() noexcept {
    _refCount++;
    return _refCount;
}

uint64_t Object::RemoveRef() noexcept {
    _refCount--;
    return _refCount;
}

}  // namespace radray
