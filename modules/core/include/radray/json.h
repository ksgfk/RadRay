#pragma once

#include <filesystem>
#include <optional>
#include <string_view>
#include <array>
#include <concepts>
#include <cstddef>
#include <span>
#include <type_traits>

#include <radray/types.h>

// yyjson 类型前置声明, 不把第三方头暴露给上层。
struct yyjson_doc;
struct yyjson_val;
struct yyjson_mut_doc;
struct yyjson_mut_val;

namespace radray {

/// 只读 JSON 节点 (轻量 view, 不拥有底层内存, 生命周期依附于 JsonDocument)。
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
    bool IsNull() const noexcept;

    /// object 成员访问。key 不存在或本节点非 object 时返回无效节点。
    JsonValue operator[](std::string_view key) const noexcept;
    bool Has(std::string_view key) const noexcept;

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
/// key 不复制，其底层字符须在 JsonWriter 生命周期内保持有效且不变；value 字符串会被复制。
class JsonRef {
public:
    JsonRef() noexcept = default;
    JsonRef(yyjson_mut_doc* doc, yyjson_mut_val* val) noexcept : _doc(doc), _val(val) {}

    bool IsValid() const noexcept { return _doc != nullptr && _val != nullptr; }

    // object 成员写入
    void AddString(std::string_view key, std::string_view value) noexcept;
    void AddUint(std::string_view key, uint64_t value) noexcept;
    void AddInt(std::string_view key, int64_t value) noexcept;
    void AddBool(std::string_view key, bool value) noexcept;
    void AddDouble(std::string_view key, double value) noexcept;
    JsonRef AddObject(std::string_view key) noexcept;
    JsonRef AddArray(std::string_view key) noexcept;

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
    friend class JsonWriteContext;

    yyjson_mut_doc* _doc{nullptr};
};

class JsonObjectWriter;
class JsonArrayWriter;

/// 指向一个尚待写入的 JSON 值。该值可以是文档根、object 成员或 array 元素。
class JsonWriteContext {
public:
    explicit JsonWriteContext(JsonWriter& writer) noexcept;
    JsonWriteContext(const JsonWriteContext&) = delete;
    JsonWriteContext& operator=(const JsonWriteContext&) = delete;
    JsonWriteContext(JsonWriteContext&&) = delete;
    JsonWriteContext& operator=(JsonWriteContext&&) = delete;

    bool Null() noexcept;
    bool String(std::string_view value) noexcept;
    bool Uint(uint64_t value) noexcept;
    bool Int(int64_t value) noexcept;
    bool Bool(bool value) noexcept;
    bool Double(double value) noexcept;

    JsonObjectWriter BeginObject() noexcept;
    JsonArrayWriter BeginArray() noexcept;

private:
    friend class JsonObjectWriter;
    friend class JsonArrayWriter;

    enum class Target : uint8_t {
        Root,
        ObjectMember,
        ArrayElement,
    };

    JsonWriteContext(
        yyjson_mut_doc* doc,
        yyjson_mut_val* parent,
        std::string_view key,
        Target target,
        bool copyKey) noexcept;

    bool Attach(yyjson_mut_val* value) noexcept;

    yyjson_mut_doc* _doc{nullptr};
    yyjson_mut_val* _parent{nullptr};
    std::string_view _key{};
    Target _target{Target::Root};
    bool _copyKey{false};
    bool _written{false};
};

/// 与 fmt::formatter<T> 类似的 JSON 写入定制点。无默认实现；支持的类型应提供特化。
template <class T>
struct JsonSerializer;

namespace detail {

template <class T>
using json_serialized_type_t = std::remove_cvref_t<T>;

}  // namespace detail

template <class T>
concept json_serializable = requires(
    JsonWriteContext& context,
    const detail::json_serialized_type_t<T>& value) {
    {
        JsonSerializer<detail::json_serialized_type_t<T>>::Write(context, value)
    } noexcept -> std::same_as<bool>;
};

template <class T>
struct is_json_serializable : std::bool_constant<json_serializable<T>> {};

template <class T>
inline constexpr bool is_json_serializable_v = is_json_serializable<T>::value;

template <json_serializable T>
bool SerializeJsonValue(JsonWriteContext& context, const T& value) noexcept {
    using value_type = detail::json_serialized_type_t<T>;
    return JsonSerializer<value_type>::Write(context, value);
}

class JsonObjectWriter {
public:
    JsonObjectWriter() noexcept = default;

    bool IsValid() const noexcept { return _doc != nullptr && _object != nullptr; }

    /// 写入静态 schema 字段。name 的底层字符须保持有效，直到 JsonWriter 完成输出。
    template <json_serializable T>
    bool Member(std::string_view name, const T& value) noexcept {
        JsonWriteContext context{
            _doc,
            _object,
            name,
            JsonWriteContext::Target::ObjectMember,
            false};
        return SerializeJsonValue(context, value);
    }

    /// 写入动态字段并复制 name，适用于 map key 或临时生成的字段名。
    template <json_serializable T>
    bool DynamicMember(std::string_view name, const T& value) noexcept {
        JsonWriteContext context{
            _doc,
            _object,
            name,
            JsonWriteContext::Target::ObjectMember,
            true};
        return SerializeJsonValue(context, value);
    }

    /// optional 无值时省略整个 object 成员。
    template <json_serializable T>
    bool OptionalMember(std::string_view name, const std::optional<T>& value) noexcept {
        return !value.has_value() || Member(name, value.value());
    }

private:
    friend class JsonWriteContext;

    JsonObjectWriter(yyjson_mut_doc* doc, yyjson_mut_val* object) noexcept
        : _doc(doc), _object(object) {}

    yyjson_mut_doc* _doc{nullptr};
    yyjson_mut_val* _object{nullptr};
};

class JsonArrayWriter {
public:
    JsonArrayWriter() noexcept = default;

    bool IsValid() const noexcept { return _doc != nullptr && _array != nullptr; }

    template <json_serializable T>
    bool Element(const T& value) noexcept {
        JsonWriteContext context{
            _doc,
            _array,
            {},
            JsonWriteContext::Target::ArrayElement,
            false};
        return SerializeJsonValue(context, value);
    }

private:
    friend class JsonWriteContext;

    JsonArrayWriter(yyjson_mut_doc* doc, yyjson_mut_val* array) noexcept
        : _doc(doc), _array(array) {}

    yyjson_mut_doc* _doc{nullptr};
    yyjson_mut_val* _array{nullptr};
};

template <class Object, class Member>
struct JsonMember {
    using object_type = Object;
    using member_type = Member;

    std::string_view Name;
    Member Object::* Pointer;
};

template <class Object, class Member>
JsonMember(std::string_view, Member Object::*) -> JsonMember<Object, Member>;

/// 简单对象的成员描述辅助函数。复杂的条件字段可直接使用 JsonObjectWriter。
template <class T, class... Members>
requires(
    (std::same_as<detail::json_serialized_type_t<T>, typename Members::object_type> && ...) &&
    (json_serializable<typename Members::member_type> && ...))
bool SerializeJsonObject(
    JsonWriteContext& context,
    const T& value,
    const Members&... members) noexcept {
    JsonObjectWriter object = context.BeginObject();
    if (!object.IsValid()) {
        return false;
    }
    return (object.Member(members.Name, value.*(members.Pointer)) && ...);
}

template <>
struct JsonSerializer<std::nullptr_t> {
    static bool Write(JsonWriteContext& context, std::nullptr_t) noexcept {
        return context.Null();
    }
};

template <>
struct JsonSerializer<bool> {
    static bool Write(JsonWriteContext& context, bool value) noexcept {
        return context.Bool(value);
    }
};

template <std::signed_integral T>
requires(!std::same_as<T, bool> && sizeof(T) <= sizeof(int64_t))
struct JsonSerializer<T> {
    static bool Write(JsonWriteContext& context, T value) noexcept {
        return context.Int(static_cast<int64_t>(value));
    }
};

template <std::unsigned_integral T>
requires(!std::same_as<T, bool> && sizeof(T) <= sizeof(uint64_t))
struct JsonSerializer<T> {
    static bool Write(JsonWriteContext& context, T value) noexcept {
        return context.Uint(static_cast<uint64_t>(value));
    }
};

template <class T>
requires(std::same_as<T, float> || std::same_as<T, double>)
struct JsonSerializer<T> {
    static bool Write(JsonWriteContext& context, T value) noexcept {
        return context.Double(static_cast<double>(value));
    }
};

template <class T>
requires std::is_enum_v<T>
struct JsonSerializer<T> {
    static bool Write(JsonWriteContext& context, T value) noexcept {
        return SerializeJsonValue(context, static_cast<std::underlying_type_t<T>>(value));
    }
};

template <>
struct JsonSerializer<std::string_view> {
    static bool Write(JsonWriteContext& context, std::string_view value) noexcept {
        return context.String(value);
    }
};

template <class Traits, class Allocator>
struct JsonSerializer<std::basic_string<char, Traits, Allocator>> {
    static bool Write(
        JsonWriteContext& context,
        const std::basic_string<char, Traits, Allocator>& value) noexcept {
        return context.String(std::string_view{value.data(), value.size()});
    }
};

template <size_t N>
struct JsonSerializer<char[N]> {
    static bool Write(JsonWriteContext& context, const char (&value)[N]) noexcept {
        size_t size = N;
        if (size > 0 && value[size - 1] == '\0') {
            --size;
        }
        return context.String(std::string_view{value, size});
    }
};

template <class T>
requires json_serializable<T>
struct JsonSerializer<std::optional<T>> {
    static bool Write(JsonWriteContext& context, const std::optional<T>& value) noexcept {
        return value.has_value() ? SerializeJsonValue(context, value.value()) : context.Null();
    }
};

template <class T, class Allocator>
requires json_serializable<T>
struct JsonSerializer<std::vector<T, Allocator>> {
    static bool Write(
        JsonWriteContext& context,
        const std::vector<T, Allocator>& value) noexcept {
        JsonArrayWriter array = context.BeginArray();
        if (!array.IsValid()) {
            return false;
        }
        if constexpr (std::same_as<T, bool>) {
            for (bool element : value) {
                if (!array.Element(element)) {
                    return false;
                }
            }
        } else {
            for (const T& element : value) {
                if (!array.Element(element)) {
                    return false;
                }
            }
        }
        return true;
    }
};

template <class T, size_t N>
requires json_serializable<T>
struct JsonSerializer<std::array<T, N>> {
    static bool Write(JsonWriteContext& context, const std::array<T, N>& value) noexcept {
        JsonArrayWriter array = context.BeginArray();
        if (!array.IsValid()) {
            return false;
        }
        for (const T& element : value) {
            if (!array.Element(element)) {
                return false;
            }
        }
        return true;
    }
};

template <class T, size_t Extent>
requires json_serializable<T>
struct JsonSerializer<std::span<T, Extent>> {
    static bool Write(JsonWriteContext& context, std::span<T, Extent> value) noexcept {
        JsonArrayWriter array = context.BeginArray();
        if (!array.IsValid()) {
            return false;
        }
        for (const T& element : value) {
            if (!array.Element(element)) {
                return false;
            }
        }
        return true;
    }
};

template <json_serializable T>
std::optional<string> SerializeJson(const T& value, bool pretty = true) noexcept {
    JsonWriter writer;
    if (!writer.IsValid()) {
        return std::nullopt;
    }

    JsonWriteContext context{writer};
    if (!SerializeJsonValue(context, value)) {
        return std::nullopt;
    }
    return writer.Write(pretty);
}

}  // namespace radray
