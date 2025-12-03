#pragma once

#include <span>
#include <string_view>
#include <limits>
#include <optional>

#include <radray/render/common.h>
#include <radray/render/utility.h>
#include <radray/render/dxc.h>
#include <radray/logger.h>

namespace radray::render {

class ShaderCBufferType;
class ShaderCBufferStorage;

class ShaderCBufferVariable {
public:
    ShaderCBufferVariable() = default;
    ShaderCBufferVariable(string name, const ShaderCBufferType* type) : _name(std::move(name)), _type(type) {}

    std::string_view GetName() const noexcept { return _name; }
    const ShaderCBufferType* GetType() const noexcept { return _type; }

private:
    string _name;
    const ShaderCBufferType* _type{nullptr};

    friend class ShaderCBufferStorage;
};

class ShaderCBufferMember {
public:
    ShaderCBufferMember() = default;
    ShaderCBufferMember(const ShaderCBufferVariable* variable, size_t offset) : _variable(variable), _offset(offset) {}

    const ShaderCBufferVariable* GetVariable() const noexcept { return _variable; }
    size_t GetOffset() const noexcept { return _offset; }

private:
    const ShaderCBufferVariable* _variable;
    size_t _offset;

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

class ShaderCBufferView;

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

        size_t AddType(std::string_view name, size_t size) noexcept;
        void AddMemberForType(size_t targetType, size_t memberType, std::string_view name, size_t offset) noexcept;
        void AddRootType(size_t typeIndex) noexcept;
        void SetAlignment(size_t align) noexcept;

        bool IsValid() const noexcept;
        std::optional<ShaderCBufferStorage> Build() noexcept;

    private:
        vector<Type> _types;
        vector<size_t> _roots;
        size_t _align{256};
    };

    ShaderCBufferView GetVar(std::string_view name) noexcept;
    void WriteData(size_t offset, const void* data, size_t size) noexcept;
    std::span<const byte> GetData() const noexcept { return _buffer; }

private:
    vector<ShaderCBufferType> _types;
    vector<ShaderCBufferVariable> _variables;
    vector<ShaderCBufferMember> _bindings;
    vector<byte> _buffer;
};

class ShaderCBufferView {
public:
    ShaderCBufferView() = default;
    ShaderCBufferView(
        ShaderCBufferStorage* storage,
        const ShaderCBufferType* type,
        size_t offset)
        : _storage(storage), _type(type), _offset(offset) {}

    bool IsValid() const noexcept { return _storage != nullptr && _type != nullptr; }
    operator bool() const noexcept { return IsValid(); }

    ShaderCBufferView GetVar(std::string_view name) const noexcept;

    template <typename T>
    void SetValue(const T& value) noexcept {
        if (!IsValid()) return;
        if (sizeof(T) > _type->GetSizeInBytes()) {
            RADRAY_ERR_LOG("ShaderCBufferView::SetValue: value size {} exceeds type size {}", sizeof(T), _type->GetSizeInBytes());
            return;
        }
        _storage->WriteData(_offset, &value, sizeof(T));
    }

private:
    ShaderCBufferStorage* _storage{nullptr};
    const ShaderCBufferType* _type{nullptr};
    size_t _offset{0};
};

std::optional<ShaderCBufferStorage> CreateCBufferStorage(std::span<const HlslShaderDesc*> descs) noexcept;

};  // namespace radray::render
