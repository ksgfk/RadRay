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
    size_t _offset{0};
};

class ShaderCBufferType {
public:
private:
    string _name;
    vector<const ShaderCBufferVariable*> _members;
    size_t _size{0};
};

class ShaderCBufferLayout {
public:
private:
    string _name;
    const ShaderCBufferType* _root{nullptr};
    size_t _size{0};
};

class ShaderCBufferStorage {
public:
private:
    vector<ShaderCBufferLayout> _layout;
    vector<std::unique_ptr<ShaderCBufferType>> _types;
    vector<std::unique_ptr<ShaderCBufferVariable>> _variables;
    vector<byte> _buffer;
};

std::optional<ShaderCBufferStorage> CreateCBufferStorage(std::span<const HlslShaderDesc*> descs) noexcept;

};  // namespace radray::render
