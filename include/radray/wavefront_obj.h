#pragma once

#include <istream>
#include <filesystem>
#include <span>

#include <radray/types.h>
#include <radray/basic_math.h>

namespace radray {

class TriangleMesh;

struct WavefrontObjFace {
    int32_t V1, V2, V3;
    int32_t Vt1, Vt2, Vt3;
    int32_t Vn1, Vn2, Vn3;
};

class WavefrontObjObject {
public:
    u8string Name;
    u8string Material;
    vector<size_t> Faces;
    bool IsSmooth;
};

class WavefrontObjReader {
public:
    struct TrianglePosition {
        Eigen::Vector3f P1, P2, P3;
    };
    struct TriangleNormal {
        Eigen::Vector3f N1, N2, N3;
    };
    struct TriangleTexcoord {
        Eigen::Vector2f UV1, UV2, UV3;
    };

    explicit WavefrontObjReader(std::istream* stream);
    explicit WavefrontObjReader(const std::filesystem::path& file);
    explicit WavefrontObjReader(string&& text);
    explicit WavefrontObjReader(const string& text);

    bool HasError() const;
    std::string_view Error() const { return _error; }
    std::span<const Eigen::Vector3f> Positions() const { return _pos; }
    std::span<const Eigen::Vector2f> UVs() const { return _uv; }
    std::span<const Eigen::Vector3f> Normals() const { return _normal; }
    std::span<const WavefrontObjFace> Faces() const { return _faces; }
    std::span<const string> Mtllibs() const { return _mtllibs; }
    std::span<const WavefrontObjObject> Objects() const { return _objects; }

    void Read();
    TrianglePosition GetPosition(size_t faceIndex) const;
    TriangleNormal GetNormal(size_t faceIndex) const;
    TriangleTexcoord GetUV(size_t faceIndex) const;

    void ToTriangleMesh(std::span<const WavefrontObjFace> faces, TriangleMesh* mesh) const;
    void ToTriangleMesh(TriangleMesh* mesh) const;
    bool ToTriangleMesh(std::u8string_view objName, TriangleMesh* mesh) const;

private:
    void Parse(std::string_view line, int lineNum);

    std::istream* _stream;
    unique_ptr<std::istream> _myStream;
    string _error;

    vector<Eigen::Vector3f> _pos;
    vector<Eigen::Vector2f> _uv;
    vector<Eigen::Vector3f> _normal;
    vector<WavefrontObjFace> _faces;
    vector<string> _mtllibs;
    vector<WavefrontObjObject> _objects;
};

}  // namespace radray
