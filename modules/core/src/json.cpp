#include <radray/json.h>

#include <cstdlib>
#include <cstring>

#include <yyjson.h>

#include <radray/file.h>

namespace radray {

// ------------------------------- JsonValue -------------------------------

bool JsonValue::IsObject() const noexcept { return yyjson_is_obj(_val); }
bool JsonValue::IsArray() const noexcept { return yyjson_is_arr(_val); }
bool JsonValue::IsString() const noexcept { return yyjson_is_str(_val); }
bool JsonValue::IsNumber() const noexcept { return yyjson_is_num(_val); }
bool JsonValue::IsBool() const noexcept { return yyjson_is_bool(_val); }

JsonValue JsonValue::operator[](const char* key) const noexcept {
    return JsonValue{yyjson_obj_get(_val, key)};
}

bool JsonValue::Has(const char* key) const noexcept {
    return yyjson_obj_get(_val, key) != nullptr;
}

size_t JsonValue::Size() const noexcept {
    if (yyjson_is_arr(_val)) {
        return yyjson_arr_size(_val);
    }
    if (yyjson_is_obj(_val)) {
        return yyjson_obj_size(_val);
    }
    return 0;
}

JsonValue JsonValue::At(size_t index) const noexcept {
    return JsonValue{yyjson_arr_get(_val, index)};
}

std::string_view JsonValue::AsString(std::string_view def) const noexcept {
    if (!yyjson_is_str(_val)) {
        return def;
    }
    const char* s = yyjson_get_str(_val);
    if (s == nullptr) {
        return def;
    }
    return std::string_view{s, yyjson_get_len(_val)};
}

uint64_t JsonValue::AsUint(uint64_t def) const noexcept {
    if (yyjson_is_uint(_val)) {
        return yyjson_get_uint(_val);
    }
    if (yyjson_is_sint(_val)) {
        int64_t v = yyjson_get_sint(_val);
        return v < 0 ? def : static_cast<uint64_t>(v);
    }
    return def;
}

int64_t JsonValue::AsInt(int64_t def) const noexcept {
    if (yyjson_is_int(_val)) {
        return yyjson_get_sint(_val);
    }
    return def;
}

double JsonValue::AsDouble(double def) const noexcept {
    if (yyjson_is_num(_val)) {
        return yyjson_get_num(_val);
    }
    return def;
}

bool JsonValue::AsBool(bool def) const noexcept {
    if (yyjson_is_bool(_val)) {
        return yyjson_get_bool(_val);
    }
    return def;
}

// ------------------------------ JsonDocument -----------------------------

JsonDocument::~JsonDocument() noexcept {
    if (_doc != nullptr) {
        yyjson_doc_free(_doc);
        _doc = nullptr;
    }
}

JsonDocument::JsonDocument(JsonDocument&& other) noexcept : _doc(other._doc) {
    other._doc = nullptr;
}

JsonDocument& JsonDocument::operator=(JsonDocument&& other) noexcept {
    if (this != &other) {
        if (_doc != nullptr) {
            yyjson_doc_free(_doc);
        }
        _doc = other._doc;
        other._doc = nullptr;
    }
    return *this;
}

std::optional<JsonDocument> JsonDocument::Parse(std::string_view text) noexcept {
    yyjson_doc* doc = yyjson_read(text.data(), text.size(), 0);
    if (doc == nullptr) {
        return std::nullopt;
    }
    return JsonDocument{doc};
}

std::optional<JsonDocument> JsonDocument::ParseFile(const std::filesystem::path& path) noexcept {
    std::optional<string> text = ReadTextFile(path);
    if (!text.has_value()) {
        return std::nullopt;
    }
    return Parse(text.value());
}

JsonValue JsonDocument::Root() const noexcept {
    return JsonValue{yyjson_doc_get_root(_doc)};
}

// ------------------------------- JsonRef ---------------------------------

void JsonRef::AddString(const char* key, std::string_view value) noexcept {
    yyjson_mut_obj_add_strncpy(_doc, _val, key, value.data(), value.size());
}

void JsonRef::AddUint(const char* key, uint64_t value) noexcept {
    yyjson_mut_obj_add_uint(_doc, _val, key, value);
}

void JsonRef::AddInt(const char* key, int64_t value) noexcept {
    yyjson_mut_obj_add_sint(_doc, _val, key, value);
}

void JsonRef::AddBool(const char* key, bool value) noexcept {
    yyjson_mut_obj_add_bool(_doc, _val, key, value);
}

void JsonRef::AddDouble(const char* key, double value) noexcept {
    yyjson_mut_obj_add_real(_doc, _val, key, value);
}

JsonRef JsonRef::AddObject(const char* key) noexcept {
    yyjson_mut_val* child = yyjson_mut_obj(_doc);
    yyjson_mut_val* k = yyjson_mut_str(_doc, key);
    yyjson_mut_obj_add(_val, k, child);
    return JsonRef{_doc, child};
}

JsonRef JsonRef::AddArray(const char* key) noexcept {
    yyjson_mut_val* child = yyjson_mut_arr(_doc);
    yyjson_mut_val* k = yyjson_mut_str(_doc, key);
    yyjson_mut_obj_add(_val, k, child);
    return JsonRef{_doc, child};
}

void JsonRef::AppendString(std::string_view value) noexcept {
    yyjson_mut_arr_add_strncpy(_doc, _val, value.data(), value.size());
}

void JsonRef::AppendUint(uint64_t value) noexcept {
    yyjson_mut_arr_add_uint(_doc, _val, value);
}

void JsonRef::AppendInt(int64_t value) noexcept {
    yyjson_mut_arr_add_sint(_doc, _val, value);
}

void JsonRef::AppendBool(bool value) noexcept {
    yyjson_mut_arr_add_bool(_doc, _val, value);
}

void JsonRef::AppendDouble(double value) noexcept {
    yyjson_mut_arr_add_real(_doc, _val, value);
}

JsonRef JsonRef::AppendObject() noexcept {
    yyjson_mut_val* child = yyjson_mut_obj(_doc);
    yyjson_mut_arr_add_val(_val, child);
    return JsonRef{_doc, child};
}

JsonRef JsonRef::AppendArray() noexcept {
    yyjson_mut_val* child = yyjson_mut_arr(_doc);
    yyjson_mut_arr_add_val(_val, child);
    return JsonRef{_doc, child};
}

// ------------------------------ JsonWriter -------------------------------

JsonWriter::JsonWriter() noexcept : _doc(yyjson_mut_doc_new(nullptr)) {}

JsonWriter::~JsonWriter() noexcept {
    if (_doc != nullptr) {
        yyjson_mut_doc_free(_doc);
        _doc = nullptr;
    }
}

JsonWriter::JsonWriter(JsonWriter&& other) noexcept : _doc(other._doc) {
    other._doc = nullptr;
}

JsonWriter& JsonWriter::operator=(JsonWriter&& other) noexcept {
    if (this != &other) {
        if (_doc != nullptr) {
            yyjson_mut_doc_free(_doc);
        }
        _doc = other._doc;
        other._doc = nullptr;
    }
    return *this;
}

JsonRef JsonWriter::RootObject() noexcept {
    yyjson_mut_val* root = yyjson_mut_obj(_doc);
    yyjson_mut_doc_set_root(_doc, root);
    return JsonRef{_doc, root};
}

JsonRef JsonWriter::RootArray() noexcept {
    yyjson_mut_val* root = yyjson_mut_arr(_doc);
    yyjson_mut_doc_set_root(_doc, root);
    return JsonRef{_doc, root};
}

std::optional<string> JsonWriter::Write(bool pretty) const noexcept {
    yyjson_write_flag flag = pretty ? YYJSON_WRITE_PRETTY : 0;
    size_t len = 0;
    char* raw = yyjson_mut_write(_doc, flag, &len);
    if (raw == nullptr) {
        return std::nullopt;
    }
    string result{raw, len};
    std::free(raw);
    return result;
}

bool JsonWriter::WriteFile(const std::filesystem::path& path, bool pretty) const noexcept {
    std::optional<string> text = Write(pretty);
    if (!text.has_value()) {
        return false;
    }
    return WriteTextFile(path, text.value());
}

}  // namespace radray
