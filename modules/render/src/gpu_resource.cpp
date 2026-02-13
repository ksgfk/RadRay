#include <radray/render/gpu_resource.h>

#include <radray/scope_guard.h>

namespace radray::render {

CBufferArena::Block::Block(unique_ptr<Buffer> buf) noexcept
    : _buf(std::move(buf)) {
    _mapped = _buf->Map(0, _buf->GetDesc().Size);
    RADRAY_ASSERT(_mapped != nullptr);
}

CBufferArena::Block::~Block() noexcept {
    if (_buf && _mapped) {
        _buf->Unmap(0, _buf->GetDesc().Size);
        _mapped = nullptr;
    }
}

CBufferArena::CBufferArena(Device* device, const Descriptor& desc) noexcept
    : _device(device),
      _desc(desc) {
    if (_desc.Alignment == 0 || (_desc.Alignment & (_desc.Alignment - 1)) != 0) {
        RADRAY_ABORT("CBufferArena invalid Alignment: {} (must be power-of-two and non-zero)", _desc.Alignment);
    }
}

CBufferArena::CBufferArena(Device* device) noexcept
    : CBufferArena(device, Descriptor{256 * 256, 256, 1024 * 1024, "cb_arena"}) {}

CBufferArena::CBufferArena(CBufferArena&& other) noexcept
    : _device(other._device),
      _blocks(std::move(other._blocks)),
      _desc(std::move(other._desc)),
      _minBlockSize(other._minBlockSize) {
    other._device = nullptr;
    other._minBlockSize = 0;
}

CBufferArena& CBufferArena::operator=(CBufferArena&& other) noexcept {
    CBufferArena tmp{std::move(other)};
    swap(*this, tmp);
    return *this;
}

CBufferArena::~CBufferArena() noexcept {
    this->Destroy();
}

bool CBufferArena::IsValid() const noexcept {
    return _device != nullptr;
}

void CBufferArena::Destroy() noexcept {
    _blocks.clear();
    _device = nullptr;
}

CBufferArena::Allocation CBufferArena::Allocate(uint64_t size) noexcept {
    if (size == 0) {
        return Allocation::Invalid();
    }
    auto blockOpt = this->GetOrCreateBlock(size);
    if (!blockOpt.HasValue()) {
        RADRAY_ABORT("allocation failed: cannot create cbuffer block");
    }
    auto block = blockOpt.Release();
    uint64_t offsetStart = Align(block->_used, _desc.Alignment);
    block->_used = offsetStart + size;
    Allocation alloc{};
    alloc.Target = block->_buf.get();
    alloc.Mapped = static_cast<byte*>(block->_mapped) + offsetStart;
    alloc.Offset = offsetStart;
    alloc.Size = size;
    return alloc;
}

Nullable<CBufferArena::Block*> CBufferArena::GetOrCreateBlock(uint64_t size) noexcept {
    if (!_blocks.empty()) {
        auto last = _blocks.back().get();
        auto desc = last->_buf->GetDesc();
        auto offsetStart = Align(last->_used, _desc.Alignment);
        if (offsetStart < desc.Size) {
            auto remain = desc.Size - offsetStart;
            if (remain >= size) {
                return last;
            }
        }
    }
    string name = radray::format("{}_{}", _desc.NamePrefix, _blocks.size());
    BufferDescriptor desc{};
    desc.Size = Align(std::max(_minBlockSize, std::max(size, _desc.BasicSize)), _desc.Alignment);
    desc.Memory = MemoryType::Upload;
    desc.Usage = BufferUse::CBuffer | BufferUse::MapWrite | BufferUse::CopySource;
    desc.Hints = ResourceHint::None;
    desc.Name = name;
    CBufferArena::Block* result = nullptr;
    {
        auto bufOpt = _device->CreateBuffer(desc);
        if (!bufOpt.HasValue()) {
            return nullptr;
        }
        result = _blocks.emplace_back(make_unique<CBufferArena::Block>(bufOpt.Release())).get();
    }
    return result;
}

void CBufferArena::Reset() noexcept {
    if (_blocks.empty()) {
        return;
    } else if (_blocks.size() == 1) {
        if (_blocks[0]->_buf->GetDesc().Size > _desc.MaxResetSize) {
            _minBlockSize = _desc.MaxResetSize;
            _blocks.clear();
        } else {
            _blocks[0]->_used = 0;
        }
    } else {
        _minBlockSize = 0;
        for (const auto& i : _blocks) {
            _minBlockSize += i->_buf->GetDesc().Size;
        }
        _minBlockSize = std::min(_minBlockSize, _desc.MaxResetSize);
        _blocks.clear();
    }
}

void CBufferArena::Clear() noexcept {
    _blocks.clear();
    _minBlockSize = 0;
}

void swap(CBufferArena& a, CBufferArena& b) noexcept {
    using std::swap;
    swap(a._device, b._device);
    swap(a._blocks, b._blocks);
    swap(a._desc, b._desc);
    swap(a._minBlockSize, b._minBlockSize);
}

}  // namespace radray::render
