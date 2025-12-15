#pragma once

#include <span>
#include <string_view>
#include <limits>
#include <optional>
#include <type_traits>

#include <radray/logger.h>
#include <radray/render/common.h>
#include <radray/render/utility.h>
#include <radray/render/dxc.h>
#include <radray/render/spvc.h>

namespace radray::render {

class ShaderCBufferType;
class ShaderCBufferStorage;
class ShaderCBufferView;

class ShaderCBufferVariable {
public:
    ShaderCBufferVariable() = default;
    ShaderCBufferVariable(string name, ShaderCBufferType* type) : _name(std::move(name)), _type(type) {}

    std::string_view GetName() const noexcept { return _name; }
    const ShaderCBufferType* GetType() const noexcept { return _type; }

private:
    string _name;
    ShaderCBufferType* _type{nullptr};

    friend class ShaderCBufferStorage;
};

class ShaderCBufferMember {
public:
    ShaderCBufferMember() = default;
    ShaderCBufferMember(ShaderCBufferVariable* variable, size_t offset) : _variable(variable), _offset(offset) {}

    const ShaderCBufferVariable* GetVariable() const noexcept { return _variable; }
    size_t GetOffset() const noexcept { return _offset; }
    uint64_t GetId() const noexcept { return _id; }
    size_t GetGlobalOffset() const noexcept { return _globalOffset; }

private:
    ShaderCBufferVariable* _variable;
    size_t _offset;
    uint64_t _id{0};
    size_t _globalOffset{0};

    friend class ShaderCBufferStorage;
};

class ShaderCBufferType {
public:
    std::string_view GetName() const noexcept { return _name; }
    std::span<const ShaderCBufferMember> GetMembers() const noexcept { return _members; }
    size_t GetSizeInBytes() const noexcept { return _size; }

private:
    string _name;
    vector<ShaderCBufferMember> _members;
    size_t _size{0};

    friend class ShaderCBufferStorage;
};

class ShaderCBufferStorage {
public:
    class Builder {
    public:
        static constexpr size_t Invalid = std::numeric_limits<size_t>::max();
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
            size_t GlobalOffset{Invalid};
        };

        size_t AddType(std::string_view name, size_t size) noexcept;
        void AddMemberForType(size_t targetType, size_t memberType, std::string_view name, size_t offset) noexcept;
        size_t AddRoot(std::string_view name, size_t typeIndex, size_t globalOffset = Invalid) noexcept;
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
    ShaderCBufferView GetVar(size_t id) noexcept;
    void WriteData(size_t offset, std::span<const byte> data) noexcept;
    std::span<const byte> GetData() const noexcept { return _buffer; }
    std::span<const byte> GetSpan(size_t offset, size_t size) const noexcept;
    std::span<const byte> GetSpan(size_t id) const noexcept;

private:
    vector<ShaderCBufferType> _types;
    vector<ShaderCBufferVariable> _variables;
    vector<ShaderCBufferMember> _bindings;
    vector<byte> _buffer;
    vector<const ShaderCBufferMember*> _idMap;

    friend class ShaderCBufferView;
};

class ShaderCBufferView {
public:
    ShaderCBufferView() = default;
    ShaderCBufferView(
        ShaderCBufferStorage* storage,
        const ShaderCBufferType* type,
        size_t offset,
        size_t id) noexcept
        : _storage(storage),
          _type(type),
          _offset(offset),
          _id(id) {}

    bool IsValid() const noexcept { return _storage != nullptr && _type != nullptr; }
    operator bool() const noexcept { return IsValid(); }

    ShaderCBufferView GetVar(std::string_view name) noexcept;
    size_t GetId() const noexcept { return _id; }
    size_t GetOffset() const noexcept { return _offset; }
    const ShaderCBufferType* GetType() const noexcept { return _type; }

    template <class T>
    void SetValue(const T& value) noexcept {
        if constexpr (std::is_trivially_copyable_v<T>) {
            constexpr size_t typeSize = sizeof(T);
            RADRAY_ASSERT(typeSize <= _type->GetSizeInBytes());
            auto dst = _storage->_buffer.data();
            auto src = reinterpret_cast<const void*>(&value);
            std::memcpy(dst + _offset, src, sizeof(T));
        } else if constexpr (IsEigenMatrix<T>::value) {
            auto dst = _storage->_buffer.data();
            auto src = value.derived().data();
            constexpr size_t byteSize = T::SizeAtCompileTime * sizeof(typename T::Scalar);
            RADRAY_ASSERT(byteSize <= _type->GetSizeInBytes());
            std::memcpy(dst + _offset, src, byteSize);
        } else if constexpr (IsEigenQuaternion<T>::value) {
            float data[4] = {value.x(), value.y(), value.z(), value.w()};
            constexpr size_t typeSize = sizeof(data);
            RADRAY_ASSERT(typeSize <= _type->GetSizeInBytes());
            auto dst = _storage->_buffer.data();
            std::memcpy(dst + _offset, data, sizeof(data));
        } else if constexpr (IsEigenTranslation<T>::value) {
            const auto& vec = value.vector();
            SetValue(vec);
        } else if constexpr (IsEigenDiagonalMatrix<T>::value) {
            const auto& vec = value.diagonal();
            SetValue(vec);
        } else {
            static_assert(false, "unsupported type");
        }
    }

private:
    ShaderCBufferStorage* _storage{nullptr};
    const ShaderCBufferType* _type{nullptr};
    size_t _offset{0};
    size_t _id{0};
};

std::optional<ShaderCBufferStorage> CreateCBufferStorage(const MergedHlslShaderDesc& desc) noexcept;

std::optional<ShaderCBufferStorage> CreateCBufferStorage(const SpirvShaderDesc& desc) noexcept;

};  // namespace radray::render
