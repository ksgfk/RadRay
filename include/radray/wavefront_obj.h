#pragma once

#include <istream>
#include <filesystem>
#include <span>

#include <radray/types.h>
#include <radray/basic_math.h>

namespace radray {

struct WavefrontObjFace {
    int32_t V1, V2, V3;
    int32_t Vt1, Vt2, Vt3;
    int32_t Vn1, Vn2, Vn3;
};

class WavefrontObjObject {
public:
    radray::string Name;
    radray::string Material;
    radray::vector<size_t> Faces;
    bool IsSmooth;
};

class WavefrontObjReader {
public:
    explicit WavefrontObjReader(std::istream* stream);
    explicit WavefrontObjReader(const std::filesystem::path& file);
    explicit WavefrontObjReader(radray::string&& text);

    bool HasError() const;
    std::string_view Error() const { return _error; }
    std::span<const Eigen::Vector3f> Positions() const { return _pos; }
    std::span<const Eigen::Vector2f> UVs() const { return _uv; }
    std::span<const Eigen::Vector3f> Normals() const { return _normal; }
    std::span<const WavefrontObjFace> Faces() const { return _faces; }
    std::span<const radray::string> Mtllibs() const { return _mtllibs; }
    std::span<const WavefrontObjObject> Objects() const { return _objects; }

    void Read();
    std::tuple<Eigen::Vector3f, Eigen::Vector3f, Eigen::Vector3f> GetPosition(size_t faceIndex) const;
    std::tuple<Eigen::Vector3f, Eigen::Vector3f, Eigen::Vector3f> GetNormal(size_t faceIndex) const;
    std::tuple<Eigen::Vector2f, Eigen::Vector2f, Eigen::Vector2f> GetUV(size_t faceIndex) const;

private:
    void Parse(std::string_view line, int lineNum);

    std::istream* _stream;
    radray::unique_ptr<std::istream> _myStream;
    radray::string _error;

    radray::vector<Eigen::Vector3f> _pos;
    radray::vector<Eigen::Vector2f> _uv;
    radray::vector<Eigen::Vector3f> _normal;
    radray::vector<WavefrontObjFace> _faces;
    radray::vector<radray::string> _mtllibs;
    radray::vector<WavefrontObjObject> _objects;
};

}  // namespace radray
