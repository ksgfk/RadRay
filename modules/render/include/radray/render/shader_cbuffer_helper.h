#pragma once

#include <span>
#include <string_view>
#include <limits>
#include <optional>
#include <type_traits>

#include <radray/logger.h>
#include <radray/render/common.h>
#include <radray/render/dxc.h>
#include <radray/render/spvc.h>

namespace radray::render {

static constexpr size_t ShaderCBufferStorageInvalidId = std::numeric_limits<size_t>::max();

class ShaderCBufferVariable;
class ShaderCBufferType;
class ShaderCBufferStorage;
class ShaderCBufferView;

class ShaderCBufferVariable {
public:
    ShaderCBufferVariable() = default;
    ShaderCBufferVariable(string name, size_t typeId) : _name(std::move(name)), _typeId(typeId) {}

private:
    string _name;
    size_t _typeId{ShaderCBufferStorageInvalidId};
    size_t _offset;

    friend class ShaderCBufferStorage;
    friend class ShaderCBufferView;
};

class ShaderCBufferType {
public:
    ShaderCBufferType() = default;
    ShaderCBufferType(std::string_view name, size_t id) : _name(string{name}), _id(id) {}

    std::string_view GetName() const noexcept { return _name; }
    size_t GetId() const noexcept { return _id; }
    size_t GetSizeInBytes() const noexcept { return _size; }

private:
    string _name;
    size_t _id{ShaderCBufferStorageInvalidId};
    vector<ShaderCBufferVariable> _members;
    size_t _size{0};

    friend class ShaderCBufferStorage;
    friend class ShaderCBufferView;
};

class ShaderCBufferStorage {
public:
    static constexpr size_t InvalidId = ShaderCBufferStorageInvalidId;

    class Builder {
    public:
        static constexpr size_t Invalid = InvalidId;
        struct Member {
            string Name;
            size_t Offset{0};
            size_t TypeIndex{Invalid};
            auto operator<=>(const Member&) const = default;
        };
        struct Type {
            string Name;
            vector<Member> Members;
            size_t SizeInBytes{0};
            auto operator<=>(const Type&) const = default;
        };
        struct Root {
            string Name;
            size_t TypeIndex{Invalid};
        };

        size_t AddType(std::string_view name, size_t size) noexcept;
        void AddMemberForType(size_t targetType, size_t memberType, std::string_view name, size_t offset) noexcept;
        size_t AddRoot(std::string_view name, size_t typeIndex) noexcept;
        void SetAlignment(size_t align) noexcept;

        bool IsValid() const noexcept;
        std::optional<ShaderCBufferStorage> Build() noexcept;

    private:
        void BuildMember(ShaderCBufferStorage& storage) noexcept;

        vector<Type> _types;
        vector<Root> _roots;
        size_t _align{0};
    };

    ShaderCBufferView GetVar(std::string_view name) noexcept;
    void WriteData(size_t offset, std::span<const byte> data) noexcept;
    std::span<const byte> GetData() const noexcept { return _buffer; }
    std::span<const byte> GetSpan(size_t offset, size_t size) const noexcept;
    std::span<const byte> GetSpan(size_t memberId) const noexcept;

private:
    struct GlobalVarIndexer {
        size_t TypeId;
        size_t MemberIndexInType;
        size_t GlobalOffset;
    };

    vector<ShaderCBufferType> _types;
    vector<ShaderCBufferVariable> _rootVarIds;
    vector<GlobalVarIndexer> _globalIndex;
    vector<byte> _buffer;

    friend class ShaderCBufferView;
};

class ShaderCBufferView {
public:
    ShaderCBufferView() = default;
    ShaderCBufferView(ShaderCBufferStorage* storage, size_t memberId) noexcept : _storage(storage), _memberId(memberId) {}

    bool IsValid() const noexcept { return _storage != nullptr && _memberId != ShaderCBufferStorageInvalidId; }
    operator bool() const noexcept { return IsValid(); }

    ShaderCBufferView GetVar(std::string_view name) noexcept;

    size_t GetId() const noexcept { return _memberId; }
    size_t GetOffset() const noexcept;
    ShaderCBufferType* GetType() noexcept;
    const ShaderCBufferType* GetType() const noexcept;

    template <class T>
    void SetValue(const T& value) noexcept {
        if (!IsValid()) {
            return;
        }
        if constexpr (std::is_trivially_copyable_v<T>) {
            _storage->WriteData(GetOffset(), std::as_bytes(std::span{&value, 1}));
        } else if constexpr (radray::IsEigenMatrix<T>::value || radray::IsEigenVector<T>::value) {
            using Scalar = typename T::Scalar;
            constexpr size_t Count = size_t(T::RowsAtCompileTime) * size_t(T::ColsAtCompileTime);
            _storage->WriteData(GetOffset(), std::as_bytes(std::span<const Scalar, Count>{value.data(), Count}));
        } else {
            static_assert(std::is_trivially_copyable_v<T>, "ShaderCBufferView::SetValue requires trivially copyable type or fixed-size Eigen matrix/vector");
        }
    }

private:
    ShaderCBufferStorage* _storage{nullptr};
    size_t _memberId{ShaderCBufferStorageInvalidId};
};

std::optional<ShaderCBufferStorage> CreateCBufferStorage(const MergedHlslShaderDesc& desc) noexcept;

std::optional<ShaderCBufferStorage> CreateCBufferStorage(const SpirvShaderDesc& desc) noexcept;

};  // namespace radray::render
