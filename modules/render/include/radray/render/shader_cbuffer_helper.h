#pragma once

#include <radray/render/common.h>
#include <radray/render/utility.h>
#include <radray/render/dxc.h>

namespace radray::render {

class ShaderCBufferType;

class ShaderCBufferVariable {
public:
private:
    string _name;
    const ShaderCBufferType* _type{nullptr};
};

class ShaderCBufferMember {
public:
private:
    const ShaderCBufferVariable* _variable;
    size_t _offset;
};

class ShaderCBufferType {
public:
private:
    string _name;
    vector<ShaderCBufferMember> _members;
    size_t _size{0};
};

class ShaderCBufferBinding {
public:
private:
    string _name;
    const ShaderCBufferType* _type{nullptr};
    size_t _size{0};
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
        };
        struct Type {
            string Name;
            vector<Member> Members;
            size_t SizeInBytes{0};
            size_t Index{Invalid};
        };

        size_t AddType(std::string_view name, size_t size) noexcept;
        void AddMemberForType(size_t targetType, size_t memberType, std::string_view name, size_t offset) noexcept;
        void AddRootType(size_t typeIndex) noexcept;
        bool IsValid() const noexcept;
        std::optional<ShaderCBufferStorage> Build() noexcept;

    private:
        vector<Type> _types;
        vector<size_t> _roots;
    };

private:
    vector<ShaderCBufferType> _types;
    vector<ShaderCBufferVariable> _variables;
    vector<ShaderCBufferBinding> _bindings;
    vector<byte> _buffer;
};

std::optional<ShaderCBufferStorage> CreateCBufferStorage(std::span<const HlslShaderDesc*> descs) noexcept;

};  // namespace radray::render
