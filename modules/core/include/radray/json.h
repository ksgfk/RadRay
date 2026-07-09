#pragma once

#include <filesystem>
#include <optional>

#include <radray/types.h>

// yyjson 类型前置声明, 不把第三方头暴露给上层。
struct yyjson_doc;
struct yyjson_val;
struct yyjson_mut_doc;
struct yyjson_mut_val;

namespace radray {

/// 只读 JSON 节点 (轻量 view, 不拥有底层内存, 生命周期依附于 JsonDocument)。
/// 所有 key 参数要求是以 null 结尾的字符串 (通常是字面量)。
class JsonValue {
public:
    JsonValue() noexcept = default;
    explicit JsonValue(yyjson_val* val) noexcept : _val(val) {}

    bool IsValid() const noexcept { return _val != nullptr; }
    bool IsObject() const noexcept;
    bool IsArray() const noexcept;
    bool IsString() const noexcept;
    bool IsNumber() const noexcept;
    bool IsBool() const noexcept;

    /// object 成员访问。key 不存在或本节点非 object 时返回无效节点。
    JsonValue operator[](const char* key) const noexcept;
    bool Has(const char* key) const noexcept;

    /// array / object 元素个数 (非容器返回 0)。
    size_t Size() const noexcept;
    /// array 下标访问 (越界或非 array 返回无效节点)。
    JsonValue At(size_t index) const noexcept;

    std::string_view AsString(std::string_view def = {}) const noexcept;
    uint64_t AsUint(uint64_t def = 0) const noexcept;
    int64_t AsInt(int64_t def = 0) const noexcept;
    double AsDouble(double def = 0.0) const noexcept;
    bool AsBool(bool def = false) const noexcept;

private:
    yyjson_val* _val{nullptr};
};

/// 拥有式只读 JSON 文档。move-only, 析构释放底层内存。
class JsonDocument {
public:
    JsonDocument() noexcept = default;
    ~JsonDocument() noexcept;
    JsonDocument(const JsonDocument&) = delete;
    JsonDocument& operator=(const JsonDocument&) = delete;
    JsonDocument(JsonDocument&& other) noexcept;
    JsonDocument& operator=(JsonDocument&& other) noexcept;

    /// 解析文本。失败返回 nullopt。
    static std::optional<JsonDocument> Parse(std::string_view text) noexcept;
    /// 读文件并解析。失败返回 nullopt。
    static std::optional<JsonDocument> ParseFile(const std::filesystem::path& path) noexcept;

    bool IsValid() const noexcept { return _doc != nullptr; }
    JsonValue Root() const noexcept;

private:
    explicit JsonDocument(yyjson_doc* doc) noexcept : _doc(doc) {}
    yyjson_doc* _doc{nullptr};
};

/// 可写 JSON 节点句柄 (指向 JsonWriter 拥有的 mutable 值)。
/// - 在 object 上用 AddXxx(key, ...)
/// - 在 array 上用 AppendXxx(...)
/// key 要求 null 结尾字符串 (通常是字面量, 不复制); value 字符串会被复制。
class JsonRef {
public:
    JsonRef() noexcept = default;
    JsonRef(yyjson_mut_doc* doc, yyjson_mut_val* val) noexcept : _doc(doc), _val(val) {}

    bool IsValid() const noexcept { return _doc != nullptr && _val != nullptr; }

    // object 成员写入
    void AddString(const char* key, std::string_view value) noexcept;
    void AddUint(const char* key, uint64_t value) noexcept;
    void AddInt(const char* key, int64_t value) noexcept;
    void AddBool(const char* key, bool value) noexcept;
    void AddDouble(const char* key, double value) noexcept;
    JsonRef AddObject(const char* key) noexcept;
    JsonRef AddArray(const char* key) noexcept;

    // array 元素追加
    void AppendString(std::string_view value) noexcept;
    void AppendUint(uint64_t value) noexcept;
    void AppendInt(int64_t value) noexcept;
    void AppendBool(bool value) noexcept;
    void AppendDouble(double value) noexcept;
    JsonRef AppendObject() noexcept;
    JsonRef AppendArray() noexcept;

private:
    yyjson_mut_doc* _doc{nullptr};
    yyjson_mut_val* _val{nullptr};
};

/// 拥有式 JSON 写入器。move-only, 析构释放底层内存。
class JsonWriter {
public:
    JsonWriter() noexcept;
    ~JsonWriter() noexcept;
    JsonWriter(const JsonWriter&) = delete;
    JsonWriter& operator=(const JsonWriter&) = delete;
    JsonWriter(JsonWriter&& other) noexcept;
    JsonWriter& operator=(JsonWriter&& other) noexcept;

    bool IsValid() const noexcept { return _doc != nullptr; }

    /// 设根为 object 并返回其句柄 (只应调用一次)。
    JsonRef RootObject() noexcept;
    /// 设根为 array 并返回其句柄 (只应调用一次)。
    JsonRef RootArray() noexcept;

    /// 序列化为字符串。pretty=true 带缩进。失败返回 nullopt。
    std::optional<string> Write(bool pretty = true) const noexcept;
    /// 序列化并写文件 (自动建父目录)。成功返回 true。
    bool WriteFile(const std::filesystem::path& path, bool pretty = true) const noexcept;

private:
    yyjson_mut_doc* _doc{nullptr};
};

}  // namespace radray
