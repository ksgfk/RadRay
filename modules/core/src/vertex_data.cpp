#include <cstring>
#include <memory>

#include <radray/vertex_data.h>

namespace radray {

MeshBuffer::MeshBuffer(std::span<const byte> data) {
    Assign(data);
}

MeshBuffer::MeshBuffer(const MeshBuffer& other) {
    Assign(other.GetData());
}

MeshBuffer& MeshBuffer::operator=(const MeshBuffer& other) {
    if (this != &other) {
        Assign(other.GetData());
    }
    return *this;
}

std::span<const byte> MeshBuffer::GetData() const noexcept {
    return std::span<const byte>{_data.get(), _size};
}

void MeshBuffer::Assign(std::span<const byte> data) {
    _size = data.size();
    if (_size == 0) {
        _data.reset();
        return;
    }

    auto buffer = std::make_unique<byte[]>(_size);
    std::memcpy(buffer.get(), data.data(), _size);
    _data = std::move(buffer);
}

}  // namespace radray
