#include <gtest/gtest.h>

#include <filesystem>
#include <string_view>

#include <radray/xml.h>

namespace radray {
namespace {

XmlElement FindElement(const XmlNode& parent, std::string_view name) {
    for (const XmlNode& child : parent.ChildNodes()) {
        if (child.NodeType() == XmlNodeType::Element && child.Name() == name) {
            return XmlElement{child};
        }
    }
    return {};
}

TEST(XmlTest, ParsesAndTraverses) {
    constexpr std::string_view kXml = R"(<?xml version="1.0"?>
<bundle version="1">
    <!-- comment -->
    <image guid="d" path="a.png"/>
    <mesh guid="e" path="b.mesh"><int name="lod" value="2"/></mesh>
</bundle>
)";

    XmlDocument doc;
    ASSERT_TRUE(doc.LoadXml(kXml, XmlParseFlag::Comments | XmlParseFlag::WhitespaceText));

    XmlElement root = doc.DocumentElement();
    ASSERT_TRUE(root.IsValid());
    EXPECT_EQ(root.NodeType(), XmlNodeType::Element);
    EXPECT_EQ(root.Name(), "bundle");
    EXPECT_TRUE(root.HasAttribute("version"));
    EXPECT_EQ(root.GetAttribute("version"), "1");

    size_t elementCount = 0;
    bool sawComment = false;
    for (const XmlNode& child : root.ChildNodes()) {
        if (child.NodeType() == XmlNodeType::Element) {
            ++elementCount;
        } else if (child.NodeType() == XmlNodeType::Comment) {
            sawComment = true;
            EXPECT_EQ(child.Name(), "#comment");
        }
    }
    EXPECT_EQ(elementCount, 2u);
    EXPECT_TRUE(sawComment);

    XmlElement image = FindElement(root, "image");
    ASSERT_TRUE(image.IsValid());
    EXPECT_EQ(image.GetAttribute("guid"), "d");
    EXPECT_EQ(image.GetAttribute("path"), "a.png");
    EXPECT_EQ(image.GetAttribute("missing"), "");

    XmlElement mesh = FindElement(root, "mesh");
    ASSERT_TRUE(mesh.IsValid());
    EXPECT_EQ(FindElement(mesh, "int").GetAttribute("value"), "2");
}

TEST(XmlTest, BuildsAndWritesRoundTrip) {
    XmlDocument doc;
    ASSERT_TRUE(doc.LoadXml("<bundle/>"));

    XmlElement root = doc.DocumentElement();
    ASSERT_TRUE(root.IsValid());

    XmlElement entry = doc.CreateElement("image");
    entry.SetAttribute("guid", "abc");
    entry.SetAttribute("path", "a.png");
    EXPECT_EQ(entry.GetAttribute("guid"), "abc");
    EXPECT_TRUE(entry.HasAttribute("path"));
    EXPECT_FALSE(entry.HasAttribute("missing"));
    EXPECT_EQ(entry.GetAttribute("missing"), "");

    root.AppendChild(entry);

    EXPECT_EQ(doc.OuterXml(XmlFormatFlag::Raw | XmlFormatFlag::NoDeclaration),
              "<bundle><image guid=\"abc\" path=\"a.png\"/></bundle>");

    XmlElement reread = FindElement(root, "image");
    ASSERT_TRUE(reread.IsValid());
    EXPECT_EQ(reread.GetAttribute("guid"), "abc");
    reread.RemoveAttribute("path");
    EXPECT_FALSE(reread.HasAttribute("path"));

    root.AppendChild(doc.CreateTextNode("hello"));
    EXPECT_EQ(root.InnerText(), "hello");
    EXPECT_EQ(root.OuterXml(XmlFormatFlag::Raw | XmlFormatFlag::NoDeclaration),
              "<bundle><image guid=\"abc\"/>hello</bundle>");
}

TEST(XmlTest, ConvertsNumbers) {
    XmlDocument doc;
    ASSERT_TRUE(doc.LoadXml("<root a=\"42\" b=\"3.5\" c=\"true\" d=\"yes\"/>"));

    XmlElement root = doc.DocumentElement();
    ASSERT_TRUE(root.IsValid());

    EXPECT_EQ(root.GetAttributeNode("a").AsInt(), 42);
    EXPECT_EQ(root.GetAttributeNode("a").AsUint(), 42u);
    EXPECT_EQ(root.GetAttributeNode("a").AsLongLong(), 42);
    EXPECT_EQ(root.GetAttributeNode("a").AsULongLong(), 42u);
    EXPECT_DOUBLE_EQ(root.GetAttributeNode("b").AsDouble(), 3.5);
    EXPECT_FLOAT_EQ(root.GetAttributeNode("b").AsFloat(), 3.5f);
    EXPECT_TRUE(root.GetAttributeNode("c").AsBool());
    EXPECT_TRUE(root.GetAttributeNode("d").AsBool());

    EXPECT_EQ(root.GetAttributeNode("missing").AsInt(), 0);
    EXPECT_EQ(root.GetAttributeNode("missing").AsInt(7), 7);
    EXPECT_FALSE(root.GetAttributeNode("missing").AsBool());

    XmlNode text = doc.CreateTextNode("99");
    EXPECT_EQ(text.AsInt(), 99);
    EXPECT_EQ(text.AsInt(1), 99);

    XmlElement num = doc.CreateElement("n");
    num.AppendChild(doc.CreateTextNode("123"));
    EXPECT_EQ(num.AsInt(), 123);
}

TEST(XmlTest, ReportsParseError) {
    XmlDocument doc;
    string error;
    EXPECT_FALSE(doc.LoadXml("<a><b></a>", {}, &error));
    EXPECT_FALSE(error.empty());
}

TEST(XmlTest, LoadsAndSavesFile) {
    const std::filesystem::path temp = std::filesystem::temp_directory_path() / "radray_test_xml_bundle.xml";
    {
        XmlDocument doc;
        ASSERT_TRUE(doc.LoadXml("<bundle version=\"1\"><image guid=\"d\" path=\"a.png\"/></bundle>"));
        ASSERT_TRUE(doc.Save(temp, XmlFormatFlag::Raw));
    }

    XmlDocument loaded;
    string error;
    ASSERT_TRUE(loaded.Load(temp, {}, &error));
    XmlElement root = loaded.DocumentElement();
    ASSERT_TRUE(root.IsValid());
    EXPECT_EQ(root.GetAttribute("version"), "1");

    std::error_code ec;
    std::filesystem::remove(temp, ec);
}

}  // namespace
}  // namespace radray
