#include <radray/wavefront_obj.h>

#include <fstream>
#include <sstream>
#include <cctype>

#include <radray/logger.h>
#include <radray/utility.h>
#include <radray/triangle_mesh.h>

namespace radray {

static bool IsStringWhiteSpace(std::string_view str) noexcept {
    for (char c : str) {
        if (c < -1 || !std::isspace(c)) {
            return false;
        }
    }
    return true;
}

static std::string_view TrimStart(std::string_view str) noexcept {
    size_t start = 0;
    for (; start < str.size(); start++) {
        auto v = str[start];
        if (v < -1 || !std::isspace(str[start])) {
            break;
        }
    }
    return str.substr(start);
}

static std::string_view TrimEnd(std::string_view str) noexcept {
    size_t end = str.size() - 1;
    for (; end >= 0; end--) {
        auto v = str[end];
        if (v < -1 || !std::isspace(str[end])) {
            break;
        }
    }
    return str.substr(0, end + 1);
}

template <size_t Count>
static std::pair<bool, size_t> ParseNumberArray(std::string_view str, std::array<float, Count>& arr) noexcept {
    std::string_view next = TrimEnd(str);
    size_t count = 0;
    while (count < arr.size()) {
        bool isEnd = false;
        size_t partEnd = next.find(' ');
        if (partEnd == std::string_view::npos) {
            isEnd = true;
        }
        std::string_view part = isEnd ? next : next.substr(0, partEnd);
        char* endPtr = nullptr;
        float result = std::strtof(part.data(), &endPtr);
        if (endPtr == part.data()) {
            return std::make_pair(false, 0);
        }
        arr[count++] = result;
        if (isEnd) {
            break;
        }
        next = TrimStart(next.substr(partEnd));
    }
    return std::make_pair(true, count);
}

static bool ParseFaceVertex(std::string_view data, int mode, int& v, int& vt, int& vn) noexcept {
    switch (mode) {
        case 1: {
            char* endPtr = nullptr;
            auto v_ = std::strtol(data.data(), &endPtr, 10);
            if (endPtr == data.data() || v_ >= std::numeric_limits<int>::max()) {
                return false;
            }
            v = static_cast<int>(v_);
            vt = 0;
            vn = 0;
            return true;
        }
        case 2: {
            size_t delimiter = data.find('/');
            if (delimiter == std::string_view::npos) {
                return false;
            }
            std::string_view vData = data.substr(0, delimiter);
            std::string_view vtData = data.substr(delimiter + 1);
            char* endPtr = nullptr;
            auto v_ = std::strtol(vData.data(), &endPtr, 10);
            if (endPtr == vData.data() || v_ >= std::numeric_limits<int>::max()) {
                return false;
            }
            endPtr = nullptr;
            auto vt_ = std::strtol(vtData.data(), &endPtr, 10);
            if (endPtr == vtData.data() || vt_ >= std::numeric_limits<int>::max()) {
                return false;
            }
            v = static_cast<int>(v_);
            vt = static_cast<int>(vt_);
            vn = 0;
            return true;
        }
        case 3: {
            size_t first = data.find('/');
            size_t second = data.find_last_of('/');
            if (first == std::string_view::npos || second == std::string_view::npos) {
                return false;
            }
            std::string_view vData = data.substr(0, first);
            std::string_view vtData = data.substr(first + 1, second - first - 1);
            std::string_view vnData = data.substr(second + 1);
            char* endPtr = nullptr;
            auto v_ = std::strtol(vData.data(), &endPtr, 10);
            if (endPtr == vData.data() || v_ >= std::numeric_limits<int>::max()) {
                return false;
            }
            endPtr = nullptr;
            auto vt_ = std::strtol(vtData.data(), &endPtr, 10);
            if (endPtr == vtData.data() || vt_ >= std::numeric_limits<int>::max()) {
                return false;
            }
            endPtr = nullptr;
            auto vn_ = std::strtol(vnData.data(), &endPtr, 10);
            if (endPtr == vnData.data() || vn_ >= std::numeric_limits<int>::max()) {
                return false;
            }
            v = static_cast<int>(v_);
            vt = static_cast<int>(vt_);
            vn = static_cast<int>(vn_);
            return true;
        }
        case 4: {
            size_t delimiter = data.find("//");
            if (delimiter == std::string_view::npos) {
                return false;
            }
            std::string_view vData = data.substr(0, delimiter);
            std::string_view vnData = data.substr(delimiter + 2);
            char* endPtr = nullptr;
            auto v_ = std::strtol(vData.data(), &endPtr, 10);
            if (endPtr == vData.data() || v_ >= std::numeric_limits<int>::max()) {
                return false;
            }
            endPtr = nullptr;
            auto vn_ = std::strtol(vnData.data(), &endPtr, 10);
            if (endPtr == vnData.data() || vn_ >= std::numeric_limits<int>::max()) {
                return false;
            }
            v = static_cast<int>(v_);
            vt = 0;
            vn = static_cast<int>(vn_);
            return true;
        }
        default:
            return false;
    }
}

static bool ParseFace(std::string_view data, WavefrontObjFace& face) noexcept {
    size_t first = data.find(' ');
    if (first == std::string_view::npos) {
        return false;
    }
    std::string_view one = TrimStart(data.substr(0, first));
    size_t firstDelimiter = one.find('/');
    /*
     * 1: f v1 v2 v3
     * 2: f v1/vt1 v2/vt2 v3/vt3
     * 3: f v1/vt1/vn1 v2/vt2/vn2 v3/vt3/vn3
     * 4: f v1//vn1 v2//vn2 v3//vn3
     */
    int mode;
    if (firstDelimiter == std::string_view::npos) {
        mode = 1;
    } else {
        size_t secondDelimiter = one.find_last_of('/');
        if (secondDelimiter == firstDelimiter) {
            mode = 2;
        } else if (secondDelimiter == firstDelimiter + 1) {
            mode = 4;
        } else {
            mode = 3;
        }
    }
    std::string_view two = TrimStart(data.substr(first));
    size_t second = two.find(' ');
    if (second == std::string_view::npos) {
        return false;
    }
    std::string_view three = TrimEnd(TrimStart(two.substr(second)));
    std::string_view realTwo = two.substr(0, second);
    if (!ParseFaceVertex(one, mode, face.V1, face.Vt1, face.Vn1)) {
        return false;
    }
    if (!ParseFaceVertex(realTwo, mode, face.V2, face.Vt2, face.Vn2)) {
        return false;
    }
    if (!ParseFaceVertex(three, mode, face.V3, face.Vt3, face.Vn3)) {
        return false;
    }
    return true;
}

template <size_t ResultCount, size_t SrcCount>
static Eigen::Vector<float, ResultCount> SelectData(const std::array<float, SrcCount>& arr, size_t full) noexcept {
    Eigen::Vector<float, ResultCount> result;
    for (size_t i = 0; i < ResultCount; i++) {
        result[i] = full < i ? 0.0f : (i < arr.size() ? arr[i] : 0.0f);
    }
    return result;
}

WavefrontObjReader::WavefrontObjReader(std::istream* stream)
    : _stream(stream) {}

WavefrontObjReader::WavefrontObjReader(const std::filesystem::path& file) {
    _myStream = radray::make_unique<std::ifstream>(file, std::ios::in);
    _stream = _myStream.get();
    if (!_stream->good()) {
        RADRAY_ERR_LOG("cannot open file: {}", file.string());
    }
}

WavefrontObjReader::WavefrontObjReader(radray::string&& text) {
    _myStream = radray::make_unique<std::basic_istringstream<char, std::char_traits<char>, radray::allocator<char>>>(std::move(text), std::ios::in, radray::allocator<char>{});
    _stream = _myStream.get();
}

bool WavefrontObjReader::HasError() const {
    return !_error.empty();
}

void WavefrontObjReader::Read() {
    if (!_stream || !_stream->good()) {
        _error = "cannot read data";
        return;
    }
    uint32_t allLine = 0;
    radray::string buffer;
    buffer.reserve(512);
    while (std::getline(*_stream, buffer)) {
        allLine++;
        if (buffer.empty()) {
            continue;
        }
        Parse(buffer, allLine);
    }
    if (_error.size() > 0 && *_error.rbegin() == '\n') {
        _error.erase(_error.begin() + _error.size() - 1);
    }
}

void WavefrontObjReader::Parse(std::string_view line, int lineNum) {
    if (IsStringWhiteSpace(line)) {
        return;
    }
    std::string_view view = TrimStart(line);
    size_t cmdEnd = view.find(' ');
    if (cmdEnd == std::string_view::npos) {
        return;
    }
    std::string_view cmd = view.substr(0, cmdEnd);
    std::string_view data = TrimStart(view.substr(cmdEnd));
    if (cmd == "v") {
        std::array<float, 4> result;
        auto [isSuccess, full] = ParseNumberArray(data, result);
        if (!isSuccess) {
            _error += radray::format("at line {}: can't parse vertex {}\n", lineNum, data);
        }
        _pos.emplace_back(SelectData<3>(result, full));
    } else if (cmd == "vt") {
        std::array<float, 3> result;
        auto [isSuccess, full] = ParseNumberArray(data, result);
        if (!isSuccess) {
            _error += fmt::format("at line {}: can't parse uv {}\n", lineNum, data);
        }
        _uv.emplace_back(SelectData<2>(result, full));
    } else if (cmd == "vn") {
        std::array<float, 3> result;
        auto [isSuccess, full] = ParseNumberArray(data, result);
        if (!isSuccess) {
            _error += fmt::format("at line {}: can't parse normal {}\n", lineNum, data);
        }
        _normal.emplace_back(SelectData<3>(result, full));
    } else if (cmd == "f") {
        WavefrontObjFace face;
        bool isSuccess = ParseFace(data, face);
        if (!isSuccess) {
            _error += fmt::format("at line {}: can't parse face {}\n", lineNum, data);
        }
        size_t index = _faces.size();
        _faces.emplace_back(face);
        if (_objects.size() > 0) {
            _objects.rbegin()->Faces.push_back(index);
        }
    } else if (cmd == "o" || cmd == "g") {
        std::string_view nameView = TrimEnd(data);
        radray::u8string name((char8_t*)nameView.data(), nameView.size());
        auto& obj = _objects.emplace_back(WavefrontObjObject{});
        obj.Name = std::move(name);
    } else if (cmd == "mtllib") {
        std::string_view v = TrimEnd(data);
        _mtllibs.emplace_back(radray::string{v});
    } else if (cmd == "usemtl") {
        if (_objects.size() > 0) {
            std::string_view v = TrimEnd(data);
            _objects.rbegin()->Material = radray::u8string{(char8_t*)v.data(), v.size()};
        }
    } else if (cmd == "s") {
        if (_objects.size() > 0) {
            std::string_view v = TrimEnd(data);
            if (v == "off") {
                _objects.rbegin()->IsSmooth = false;
            } else if (v == "on") {
                _objects.rbegin()->IsSmooth = true;
            } else if (v == "1") {
                _objects.rbegin()->IsSmooth = true;
            } else if (v == "0") {
                _objects.rbegin()->IsSmooth = false;
            } else {
                _error += fmt::format("at line {}: unknown smooth value {}\n", lineNum, v);
            }
        }
    } else if (cmd == "#") {
        // no op
    } else {
        _error += fmt::format("at line {}: unknown command {}\n", lineNum, cmd);
    }
}

static size_t CvtIdx(int f, size_t count) noexcept {
    return f >= 0 ? f - 1 : count + f;
}

WavefrontObjReader::TrianglePosition WavefrontObjReader::GetPosition(size_t faceIndex) const {
    const WavefrontObjFace& f = _faces[faceIndex];
    size_t count = _pos.size();
    size_t a = CvtIdx(f.V1, count), b = CvtIdx(f.V2, count), c = CvtIdx(f.V3, count);
    return {_pos[a], _pos[b], _pos[c]};
}

WavefrontObjReader::TriangleNormal WavefrontObjReader::GetNormal(size_t faceIndex) const {
    const WavefrontObjFace& f = _faces[faceIndex];
    size_t count = _normal.size();
    size_t a = CvtIdx(f.Vn1, count), b = CvtIdx(f.Vn2, count), c = CvtIdx(f.Vn3, count);
    return {_normal[a], _normal[b], _normal[c]};
}

WavefrontObjReader::TriangleTexcoord WavefrontObjReader::GetUV(size_t faceIndex) const {
    const WavefrontObjFace& f = _faces[faceIndex];
    size_t count = _uv.size();
    size_t a = CvtIdx(f.Vt1, count), b = CvtIdx(f.Vt2, count), c = CvtIdx(f.Vt3, count);
    return {_uv[a], _uv[b], _uv[c]};
}

struct VertexCombine {
    int32_t P, N, T;

    bool operator==(const VertexCombine& other) const noexcept {
        return P == other.P && N == other.N && T == other.T;
    }
};

struct VertexCombineHash {
    size_t operator()(const VertexCombine& v) const noexcept {
        return radray::HashData(&v, sizeof(v));
    }
};

void WavefrontObjReader::ToTriangleMesh(std::span<const WavefrontObjFace> faces, TriangleMesh* mesh) const {
    unordered_map<VertexCombine, uint32_t, VertexCombineHash> unique;
    vector<Eigen::Vector3f> p;
    vector<Eigen::Vector3f> n;
    vector<Eigen::Vector2f> u;
    vector<uint32_t> ind;
    uint32_t count = 0;
    VertexCombine v[3];
    for (const WavefrontObjFace& f : faces) {
        v[0] = VertexCombine{f.V1, f.Vn1, f.Vt1};
        v[1] = VertexCombine{f.V2, f.Vn2, f.Vt2};
        v[2] = VertexCombine{f.V3, f.Vn3, f.Vt3};
        for (size_t j = 0; j < 3; j++) {
            auto iter = unique.find(v[j]);
            uint32_t index;
            if (iter == unique.end()) {
                unique.emplace(v[j], count);
                index = count;
                p.push_back(_pos[CvtIdx(v[j].P, _pos.size())]);
                if (v[j].N != 0) {
                    n.push_back(_normal[CvtIdx(v[j].N, _normal.size())]);
                }
                if (v[j].T != 0) {
                    u.push_back(_uv[CvtIdx(v[j].T, _uv.size())]);
                }
                count++;
            } else {
                index = iter->second;
            }
            ind.push_back(index);
        }
    }

    mesh->Indices = std::move(ind);
    mesh->Positions = std::move(p);
    mesh->Normals = std::move(n);
    mesh->UV0 = std::move(u);
}

void WavefrontObjReader::ToTriangleMesh(TriangleMesh* mesh) const {
    this->ToTriangleMesh(_faces, mesh);
}

bool WavefrontObjReader::ToTriangleMesh(std::u8string_view objName, TriangleMesh* mesh) const {
    for (const auto& obj : _objects) {
        if (obj.Name == objName) {
            vector<WavefrontObjFace> faces;
            faces.reserve(obj.Faces.size());
            for (size_t i : obj.Faces) {
                faces.push_back(_faces[i]);
            }
            this->ToTriangleMesh(faces, mesh);
            return true;
        }
    }
    return false;
}

}  // namespace radray
