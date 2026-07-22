#include <radray/json.h>

#include <cstdlib>

#include <yyjson.h>

#include <radray/file.h>

namespace radray {
namespace {

const char* StringViewData(std::string_view value) noexcept {
    return value.data() != nullptr ? value.data() : "";
}

yyjson_mut_val* MakeKey(yyjson_mut_doc* doc, std::string_view key) noexcept {
    return yyjson_mut_strn(doc, StringViewData(key), key.size());
}

void AddMember(yyjson_mut_doc* doc,
               yyjson_mut_val* object,
               std::string_view key,
               yyjson_mut_val* value) noexcept {
    yyjson_mut_obj_add(object, MakeKey(doc, key), value);
}

}  // namespace

// ------------------------------- JsonValue -------------------------------

bool JsonValue::IsObject() const noexcept { return yyjson_is_obj(_val); }
bool JsonValue::IsArray() const noexcept { return yyjson_is_arr(_val); }
bool JsonValue::IsString() const noexcept { return yyjson_is_str(_val); }
bool JsonValue::IsNumber() const noexcept { return yyjson_is_num(_val); }
bool JsonValue::IsBool() const noexcept { return yyjson_is_bool(_val); }
bool JsonValue::IsNull() const noexcept { return yyjson_is_null(_val); }

JsonValue JsonValue::operator[](std::string_view key) const noexcept {
    return JsonValue{yyjson_obj_getn(_val, StringViewData(key), key.size())};
}

bool JsonValue::Has(std::string_view key) const noexcept {
    return yyjson_obj_getn(_val, StringViewData(key), key.size()) != nullptr;
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

void JsonRef::AddString(std::string_view key, std::string_view value) noexcept {
    yyjson_mut_val* val = yyjson_mut_strncpy(_doc, StringViewData(value), value.size());
    AddMember(_doc, _val, key, val);
}

void JsonRef::AddUint(std::string_view key, uint64_t value) noexcept {
    AddMember(_doc, _val, key, yyjson_mut_uint(_doc, value));
}

void JsonRef::AddInt(std::string_view key, int64_t value) noexcept {
    AddMember(_doc, _val, key, yyjson_mut_sint(_doc, value));
}

void JsonRef::AddBool(std::string_view key, bool value) noexcept {
    AddMember(_doc, _val, key, yyjson_mut_bool(_doc, value));
}

void JsonRef::AddDouble(std::string_view key, double value) noexcept {
    AddMember(_doc, _val, key, yyjson_mut_real(_doc, value));
}

JsonRef JsonRef::AddObject(std::string_view key) noexcept {
    yyjson_mut_val* child = yyjson_mut_obj(_doc);
    AddMember(_doc, _val, key, child);
    return JsonRef{_doc, child};
}

JsonRef JsonRef::AddArray(std::string_view key) noexcept {
    yyjson_mut_val* child = yyjson_mut_arr(_doc);
    AddMember(_doc, _val, key, child);
    return JsonRef{_doc, child};
}

void JsonRef::AppendString(std::string_view value) noexcept {
    yyjson_mut_arr_add_strncpy(_doc, _val, StringViewData(value), value.size());
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

JsonWriteContext::JsonWriteContext(JsonWriter& writer) noexcept
    : _doc(writer._doc) {}

JsonWriteContext::JsonWriteContext(yyjson_mut_doc* doc,
                                   yyjson_mut_val* parent,
                                   std::string_view key,
                                   Target target,
                                   bool copyKey) noexcept
    : _doc(doc),
      _parent(parent),
      _key(key),
      _target(target),
      _copyKey(copyKey) {}

bool JsonWriteContext::Attach(yyjson_mut_val* value) noexcept {
    if (_written || _doc == nullptr || value == nullptr) {
        return false;
    }

    bool success = false;
    switch (_target) {
        case Target::Root:
            if (yyjson_mut_doc_get_root(_doc) == nullptr) {
                yyjson_mut_doc_set_root(_doc, value);
                success = true;
            }
            break;
        case Target::ObjectMember: {
            yyjson_mut_val* key = _copyKey
                                      ? yyjson_mut_strncpy(
                                            _doc,
                                            StringViewData(_key),
                                            _key.size())
                                      : yyjson_mut_strn(
                                            _doc,
                                            StringViewData(_key),
                                            _key.size());
            success = yyjson_mut_obj_add(_parent, key, value);
            break;
        }
        case Target::ArrayElement:
            success = yyjson_mut_arr_add_val(_parent, value);
            break;
    }

    _written = success;
    return success;
}

bool JsonWriteContext::Null() noexcept {
    return Attach(yyjson_mut_null(_doc));
}

bool JsonWriteContext::String(std::string_view value) noexcept {
    return Attach(yyjson_mut_strncpy(
        _doc,
        StringViewData(value),
        value.size()));
}

bool JsonWriteContext::Uint(uint64_t value) noexcept {
    return Attach(yyjson_mut_uint(_doc, value));
}

bool JsonWriteContext::Int(int64_t value) noexcept {
    return Attach(yyjson_mut_sint(_doc, value));
}

bool JsonWriteContext::Bool(bool value) noexcept {
    return Attach(yyjson_mut_bool(_doc, value));
}

bool JsonWriteContext::Double(double value) noexcept {
    return std::isfinite(value) && Attach(yyjson_mut_real(_doc, value));
}

JsonObjectWriter JsonWriteContext::BeginObject() noexcept {
    yyjson_mut_val* object = yyjson_mut_obj(_doc);
    if (!Attach(object)) {
        return {};
    }
    return JsonObjectWriter{_doc, object};
}

JsonArrayWriter JsonWriteContext::BeginArray() noexcept {
    yyjson_mut_val* array = yyjson_mut_arr(_doc);
    if (!Attach(array)) {
        return {};
    }
    return JsonArrayWriter{_doc, array};
}

}  // namespace radray
