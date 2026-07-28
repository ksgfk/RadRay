#pragma once

#include <radray/json.h>
#include <radray/shader/shader_manifest.h>

// manifest 字段写入的单一实现。
//
// 【为何抽出】: 生成的模板需要在同一个 JSON 对象里额外带一个 "_TODO" 成员, 而
// JsonSerializer<ShaderAssetDesc>::Write 自己调用 BeginObject, 外部无法往里插字段。
// 若让模板的序列化器自行罗列一遍 manifest 字段, 两份列表迟早分叉 (加了新字段只改
// 一处), 生成出的模板就会静默丢字段。故把"往已开好的 object 里写 manifest 字段"
// 单独成一个函数, 普通 codec 与模板 codec 共用。

namespace radray {

/// 往【已开始的】 object 写入 manifest 的全部字段 (含 FormatVersion)。
/// 调用方负责 BeginObject 并可在前后追加自己的成员。
bool WriteShaderAssetMembers(JsonObjectWriter& object, const ShaderAssetDesc& value) noexcept;

}  // namespace radray
