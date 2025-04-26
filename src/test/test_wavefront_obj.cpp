#include <gtest/gtest.h>

#include <fstream>

#include <radray/wavefront_obj.h>

const char* objFile = R"(# Blender 4.4.1
# www.blender.org
o Floor
v 0.000000 0.000000 0.000000
v 5.528000 0.000000 0.000000
v 0.000000 0.000000 -5.592000
v 5.496000 0.000000 -5.592000
vn -0.0000 1.0000 -0.0000
vt 0.000000 0.000000
vt 1.000000 1.000000
vt 0.000000 1.000000
vt 1.000000 0.000000
s 0
f 1/1/1 4/2/1 3/3/1
f 1/1/1 2/4/1 4/2/1)";

const char* testFile = "___test___.obj";

class WaveObjTest : public testing::Test {
protected:
    virtual void SetUp() override {
        std::ofstream file(testFile, std::ios::out | std::ios::trunc);
        if (!file.is_open() || !file.good()) {
            FAIL() << "Cannot open file for writing";
        }
        file << objFile;
        file.close();
    }

    virtual void TearDown() override {
        bool isSucc = std::filesystem::remove(testFile);
        if (!isSucc) {
            FAIL() << "Cannot remove test file";
        }
    }
};

static void SimpleTest(radray::WavefrontObjReader& reader) {
    EXPECT_FALSE(reader.HasError()) << reader.Error();
    EXPECT_EQ(reader.Positions().size(), 4);
    EXPECT_EQ(reader.UVs().size(), 4);
    EXPECT_EQ(reader.Normals().size(), 1);
    EXPECT_EQ(reader.Faces().size(), 2);

    EXPECT_FLOAT_EQ(reader.Positions()[1].x(), 5.528f);
    EXPECT_FLOAT_EQ(reader.Positions()[2].z(), -5.592f);
    EXPECT_FLOAT_EQ(reader.UVs()[1].x(), 1.0f);
    EXPECT_FLOAT_EQ(reader.UVs()[1].y(), 1.0f);
    EXPECT_FLOAT_EQ(reader.UVs()[2].y(), 1.0f);

    auto f1 = reader.Faces()[0];
    EXPECT_EQ(f1.V1, 1);
    EXPECT_EQ(f1.V2, 4);
    EXPECT_EQ(f1.V3, 3);
    EXPECT_EQ(f1.Vt1, 1);
    EXPECT_EQ(f1.Vt2, 2);
    EXPECT_EQ(f1.Vt3, 3);
    EXPECT_EQ(f1.Vn1, 1);
    EXPECT_EQ(f1.Vn2, 1);
    EXPECT_EQ(f1.Vn3, 1);
}

TEST(Core_WaveObjTest, Simple) {
    radray::WavefrontObjReader reader{radray::string(objFile)};
    reader.Read();
    SimpleTest(reader);
}

TEST_F(WaveObjTest, SimpleFile) {
    std::filesystem::path path{testFile};
    radray::WavefrontObjReader reader{path};
    reader.Read();
    SimpleTest(reader);
}
