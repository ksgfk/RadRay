#include <gtest/gtest.h>

#include <radray/triangle_mesh.h>
#include <radray/wavefront_obj.h>

using namespace radray;

namespace {

constexpr const char* QuadObj = R"(# quad split into two triangles
mtllib scene.mtl
o Floor
usemtl Stone
s off
v 0 0 0
v 2 0 0
v 0 0 -3
v 2 0 -3
vt 0 0
vt 1 1
vt 0 1
vt 1 0
vn 0 1 0
f 1/1/1 4/2/1 3/3/1
f 1/1/1 2/4/1 4/2/1
)";

WavefrontObjReader ReadText(const char* text) {
    WavefrontObjReader reader{string{text}};
    reader.Read();
    return reader;
}

}  // namespace

TEST(WavefrontObjTest, ParsesVertexAttributesAndFaces) {
    const auto reader = ReadText(QuadObj);

    ASSERT_FALSE(reader.HasError()) << reader.Error();
    ASSERT_EQ(reader.Positions().size(), 4u);
    ASSERT_EQ(reader.UVs().size(), 4u);
    ASSERT_EQ(reader.Normals().size(), 1u);
    ASSERT_EQ(reader.Faces().size(), 2u);
    EXPECT_FLOAT_EQ(reader.Positions()[3].z(), -3.0f);
    EXPECT_FLOAT_EQ(reader.UVs()[1].x(), 1.0f);
    const WavefrontObjFace face = reader.Faces()[0];
    EXPECT_EQ(face.V2, 4);
    EXPECT_EQ(face.Vt2, 2);
    EXPECT_EQ(face.Vn2, 1);
}

TEST(WavefrontObjTest, GroupsFacesIntoObjectsWithMaterial) {
    const auto reader = ReadText(QuadObj);

    ASSERT_EQ(reader.Mtllibs().size(), 1u);
    EXPECT_EQ(reader.Mtllibs()[0], "scene.mtl");
    ASSERT_EQ(reader.Objects().size(), 1u);
    const WavefrontObjObject& object = reader.Objects()[0];
    EXPECT_EQ(object.Name, u8"Floor");
    EXPECT_EQ(object.Material, u8"Stone");
    EXPECT_FALSE(object.IsSmooth);
    EXPECT_EQ(object.Faces, (vector<size_t>{0, 1}));
}

TEST(WavefrontObjTest, ParsesAllFaceVertexForms) {
    const auto reader = ReadText(
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvn 0 0 1\n"
        "f 1 2 3\n"
        "f 1/1 2/1 3/1\n"
        "f 1//1 2//1 3//1\n");

    ASSERT_FALSE(reader.HasError()) << reader.Error();
    ASSERT_EQ(reader.Faces().size(), 3u);
    EXPECT_EQ(reader.Faces()[0].V3, 3);
    EXPECT_EQ(reader.Faces()[0].Vt3, 0);
    EXPECT_EQ(reader.Faces()[1].Vt3, 1);
    EXPECT_EQ(reader.Faces()[1].Vn3, 0);
    EXPECT_EQ(reader.Faces()[2].Vt3, 0);
    EXPECT_EQ(reader.Faces()[2].Vn3, 1);
}

TEST(WavefrontObjTest, NegativeIndicesCountFromEnd) {
    const auto reader = ReadText("v 1 0 0\nv 2 0 0\nv 3 0 0\nf -3 -2 -1\n");
    ASSERT_FALSE(reader.HasError()) << reader.Error();

    const auto triangle = reader.GetPosition(0);

    EXPECT_FLOAT_EQ(triangle.P1.x(), 1.0f);
    EXPECT_FLOAT_EQ(triangle.P2.x(), 2.0f);
    EXPECT_FLOAT_EQ(triangle.P3.x(), 3.0f);
}

TEST(WavefrontObjTest, ReportsUnknownCommandWithLineNumber) {
    const auto reader = ReadText("v 0 0 0\nbogus 1 2\n");

    ASSERT_TRUE(reader.HasError());
    EXPECT_NE(reader.Error().find("line 2"), std::string_view::npos) << reader.Error();
    EXPECT_NE(reader.Error().find("bogus"), std::string_view::npos) << reader.Error();
}

TEST(WavefrontObjTest, ReportsMalformedVertex) {
    const auto reader = ReadText("v 0 x 0\n");

    EXPECT_TRUE(reader.HasError());
}

TEST(WavefrontObjTest, ToTriangleMeshSharesIdenticalVertices) {
    const auto reader = ReadText(QuadObj);
    TriangleMesh mesh{};

    reader.ToTriangleMesh(&mesh);

    EXPECT_EQ(mesh.Positions.size(), 4u);
    EXPECT_EQ(mesh.UV0.size(), 4u);
    EXPECT_EQ(mesh.Normals.size(), 4u);
    EXPECT_EQ(mesh.Indices, (vector<uint32_t>{0, 1, 2, 0, 3, 1}));
    EXPECT_FLOAT_EQ(mesh.Positions[1].x(), 2.0f);
    EXPECT_FLOAT_EQ(mesh.Positions[1].z(), -3.0f);
}

TEST(WavefrontObjTest, ToTriangleMeshByNameSelectsObjectFaces) {
    const auto reader = ReadText(
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
        "o A\nf 1 2 3\n"
        "o B\nf 1 2 4\n");
    TriangleMesh mesh{};

    ASSERT_TRUE(reader.ToTriangleMesh(u8"B", &mesh));

    ASSERT_EQ(mesh.Positions.size(), 3u);
    EXPECT_FLOAT_EQ(mesh.Positions[2].z(), 1.0f);
    EXPECT_FALSE(reader.ToTriangleMesh(u8"Missing", &mesh));
}
