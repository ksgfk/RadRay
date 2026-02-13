#pragma once

#include <stdexcept>

#include <radray/basic_corotinue.h>
#include <radray/vertex_data.h>
#include <radray/render/common.h>

namespace radray::render {

class GpuResourceException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class RenderMesh {
public:
    struct DrawData {
        VertexBufferView Vbv;
        IndexBufferView Ibv;
    };

    vector<unique_ptr<Buffer>> _buffers;
    vector<DrawData> _drawDatas;
};

class CBufferArena {
public:
    struct Descriptor {
        uint64_t BasicSize{256 * 256};
        uint64_t Alignment{256};
        uint64_t MaxResetSize{std::numeric_limits<uint64_t>::max()};
        string NamePrefix{};
    };

    struct Allocation {
        Buffer* Target{nullptr};
        void* Mapped{nullptr};
        uint64_t Offset{0};
        uint64_t Size{0};

        static constexpr Allocation Invalid() noexcept { return Allocation{}; }
    };

    class Block {
    public:
        explicit Block(unique_ptr<Buffer> buf) noexcept;
        Block(const Block&) = delete;
        Block& operator=(const Block&) = delete;
        ~Block() noexcept;

    public:
        unique_ptr<Buffer> _buf;
        void* _mapped{nullptr};
        uint64_t _used{0};
    };

    CBufferArena(Device* device, const Descriptor& desc) noexcept;
    explicit CBufferArena(Device* device) noexcept;
    CBufferArena(const CBufferArena&) = delete;
    CBufferArena& operator=(const CBufferArena&) = delete;
    CBufferArena(CBufferArena&& other) noexcept;
    CBufferArena& operator=(CBufferArena&& other) noexcept;
    ~CBufferArena() noexcept;

    bool IsValid() const noexcept;

    void Destroy() noexcept;

    Allocation Allocate(uint64_t size) noexcept;

    void Reset() noexcept;

    void Clear() noexcept;

    friend void swap(CBufferArena& a, CBufferArena& b) noexcept;

private:
    Nullable<Block*> GetOrCreateBlock(uint64_t size) noexcept;

    Device* _device;
    vector<unique_ptr<Block>> _blocks;
    Descriptor _desc;
    uint64_t _minBlockSize{};
};

}  // namespace radray::render
