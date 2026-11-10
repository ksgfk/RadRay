#include <radray/xml.h>

#include <radray/logger.h>

#include <filesystem>
#include <system_error>

#include <gtest/gtest.h>

namespace radray {
namespace {

constexpr EnumFlags<XmlFormatFlag> CompactFormat = XmlFormatFlag::Raw | XmlFormatFlag::NoDeclaration;

class ScopedTempFile {
public:
    explicit ScopedTempFile(std::string_view name)
        : _path(std::filesystem::temp_directory_path() / radray::format("radray_core_xml_{}", name)) {}
    ScopedTempFile(const ScopedTempFile&) = delete;
    ScopedTempFile& operator=(const ScopedTempFile&) = delete;
    ~ScopedTempFile() noexcept {
        std::error_code ec;
        std::filesystem::remove(_path, ec);
    }

    const std::filesystem::path& Path() const noexcept { return _path; }

private:
    std::filesystem::path _path;
};

TEST(XmlTest, ParsesElementsAndAttributes) {
    XmlDocument doc;
    ASSERT_TRUE(doc.LoadXml(R"(<bundle version="1"><image path="a.png"/><image path="b.png"/><mesh/></bundle>)"));

    const XmlElement root = doc.DocumentElement();

    ASSERT_TRUE(root.IsValid());
    EXPECT_EQ(root.Name(), "bundle");
    EXPECT_EQ(root.GetAttribute("version"), "1");
    EXPECT_EQ(root.GetAttribute("missing"), "");
    EXPECT_FALSE(root.HasAttribute("missing"));
    const vector<XmlElement> images = root.Children("image");
    ASSERT_EQ(images.size(), 2u);
    EXPECT_EQ(images[1].GetAttribute("path"), "b.png");
    EXPECT_EQ(root.Child("image"), images[0]);
    EXPECT_TRUE(root.Child("mesh").IsValid());
    EXPECT_FALSE(root.Child("missing").IsValid());
}

TEST(XmlTest, TraversesSiblingsAndParents) {
    XmlDocument doc;
    ASSERT_TRUE(doc.LoadXml("<root><a/><b/></root>"));
    const XmlElement root = doc.DocumentElement();

    const XmlNode a = root.FirstChild();
    const XmlNode b = root.LastChild();

    EXPECT_EQ(a.Name(), "a");
    EXPECT_EQ(b.Name(), "b");
    EXPECT_EQ(a.NextSibling(), b);
    EXPECT_EQ(b.PreviousSibling(), a);
    EXPECT_FALSE(b.NextSibling().IsValid());
    EXPECT_EQ(a.ParentNode(), root);
    EXPECT_EQ(root.ParentNode().NodeType(), XmlNodeType::Document);
    EXPECT_FALSE(a.HasChildNodes());
}

TEST(XmlTest, CommentsAreSkippedUnlessRequested) {
    constexpr std::string_view Xml = "<root><!--note--><a/></root>";
    XmlDocument plain;
    XmlDocument withComments;
    ASSERT_TRUE(plain.LoadXml(Xml));
    ASSERT_TRUE(withComments.LoadXml(Xml, XmlParseFlag::Comments));

    EXPECT_EQ(plain.DocumentElement().ChildNodes().size(), 1u);
    const XmlNode comment = withComments.DocumentElement().FirstChild();
    EXPECT_EQ(comment.NodeType(), XmlNodeType::Comment);
    EXPECT_EQ(comment.Name(), "#comment");
    EXPECT_EQ(comment.Value(), "note");
}

TEST(XmlTest, InnerTextConcatenatesDescendantTextAndCData) {
    XmlDocument doc;
    ASSERT_TRUE(doc.LoadXml("<root>a<b>b<![CDATA[<c>]]></b>d</root>"));

    EXPECT_EQ(doc.DocumentElement().InnerText(), "ab<c>d");
}

TEST(XmlTest, BuiltDocumentSerializes) {
    XmlDocument doc;
    XmlElement root = doc.CreateElement("bundle");
    XmlElement image = root.AppendChild("image");
    image.SetAttribute("path", "a.png");
    image.SetAttribute("path", "b.png");
    root.AppendChild(doc.CreateTextNode("x<y"));

    EXPECT_EQ(doc.DocumentElement(), root);
    EXPECT_EQ(doc.OuterXml(CompactFormat), R"(<bundle><image path="b.png"/>x&lt;y</bundle>)");
    EXPECT_EQ(root.InnerXml(CompactFormat), R"(<image path="b.png"/>x&lt;y)");
}

TEST(XmlTest, AppendChildMovesNodeFromPreviousParent) {
    XmlDocument doc;
    ASSERT_TRUE(doc.LoadXml("<root><from><item/></from><to/></root>"));
    XmlElement root = doc.DocumentElement();
    XmlElement item = root.Child("from").Child("item");

    root.Child("to").AppendChild(item);

    EXPECT_EQ(root.OuterXml(CompactFormat), "<root><from/><to><item/></to></root>");
}

TEST(XmlTest, PrependChildInsertsFirst) {
    XmlDocument doc;
    ASSERT_TRUE(doc.LoadXml("<root><b/></root>"));
    XmlElement root = doc.DocumentElement();

    root.PrependChild(doc.CreateElement("a"));

    EXPECT_EQ(root.OuterXml(CompactFormat), "<root><a/><b/></root>");
}

TEST(XmlTest, RemovesChildrenAndAttributes) {
    XmlDocument doc;
    ASSERT_TRUE(doc.LoadXml(R"(<root x="1" y="2"><a/><b/></root>)"));
    XmlElement root = doc.DocumentElement();

    EXPECT_TRUE(root.RemoveChild(root.Child("a")));
    EXPECT_TRUE(root.RemoveAttribute("x"));
    EXPECT_FALSE(root.RemoveAttribute("x"));
    EXPECT_EQ(root.OuterXml(CompactFormat), R"(<root y="2"><b/></root>)");

    root.RemoveAll();
    root.RemoveAllAttributes();
    EXPECT_EQ(root.OuterXml(CompactFormat), "<root/>");
}

TEST(XmlTest, AttributeConversionsFallBackToDefault) {
    XmlDocument doc;
    ASSERT_TRUE(doc.LoadXml(R"(<root i="-42" u="42" big="9000000000" f="3.5" t="yes" n="no"/>)"));
    const XmlElement root = doc.DocumentElement();

    EXPECT_EQ(root.GetAttributeNode("i").AsInt(), -42);
    EXPECT_EQ(root.GetAttributeNode("u").AsUint(), 42u);
    EXPECT_EQ(root.GetAttributeNode("big").AsLongLong(), 9000000000LL);
    EXPECT_EQ(root.GetAttributeNode("big").AsULongLong(), 9000000000ULL);
    EXPECT_FLOAT_EQ(root.GetAttributeNode("f").AsFloat(), 3.5f);
    EXPECT_DOUBLE_EQ(root.GetAttributeNode("f").AsDouble(), 3.5);
    EXPECT_TRUE(root.GetAttributeNode("t").AsBool());
    EXPECT_FALSE(root.GetAttributeNode("n").AsBool(true));
    EXPECT_FALSE(root.GetAttributeNode("missing").IsValid());
    EXPECT_EQ(root.GetAttributeNode("missing").AsInt(7), 7);
    EXPECT_TRUE(root.GetAttributeNode("missing").AsBool(true));
}

TEST(XmlTest, AttributeSetValueUpdatesElement) {
    XmlDocument doc;
    ASSERT_TRUE(doc.LoadXml(R"(<root a="1"/>)"));
    XmlElement root = doc.DocumentElement();

    root.GetAttributeNode("a").SetValue("2");

    const vector<XmlAttribute> attributes = root.Attributes();
    ASSERT_EQ(attributes.size(), 1u);
    EXPECT_EQ(attributes[0].Name(), "a");
    EXPECT_EQ(attributes[0].Value(), "2");
}

TEST(XmlTest, ElementTextConvertsToNumber) {
    XmlDocument doc;
    ASSERT_TRUE(doc.LoadXml("<root><n>123</n><e/></root>"));
    const XmlElement root = doc.DocumentElement();

    EXPECT_EQ(root.Child("n").AsInt(), 123);
    EXPECT_EQ(root.Child("e").AsInt(5), 5);
}

TEST(XmlTest, MalformedXmlReportsError) {
    XmlDocument doc;
    string error;

    EXPECT_FALSE(doc.LoadXml("<a><b></a>", {}, &error));

    EXPECT_FALSE(error.empty());
}

TEST(XmlTest, ResetClearsDocument) {
    XmlDocument doc;
    ASSERT_TRUE(doc.LoadXml("<root/>"));

    doc.Reset();

    EXPECT_FALSE(doc.DocumentElement().IsValid());
}

TEST(XmlTest, SavedFileLoadsBack) {
    const ScopedTempFile file{"round_trip.xml"};
    {
        XmlDocument doc;
        ASSERT_TRUE(doc.LoadXml(R"(<bundle version="1"><image path="a.png"/></bundle>)"));
        ASSERT_TRUE(doc.Save(file.Path(), XmlFormatFlag::Raw));
    }

    XmlDocument loaded;
    string error;
    ASSERT_TRUE(loaded.Load(file.Path(), {}, &error)) << error;

    EXPECT_EQ(loaded.DocumentElement().Child("image").GetAttribute("path"), "a.png");
}

TEST(XmlTest, LoadMissingFileReportsError) {
    const ScopedTempFile file{"missing.xml"};
    XmlDocument doc;
    string error;

    EXPECT_FALSE(doc.Load(file.Path(), {}, &error));

    EXPECT_FALSE(error.empty());
}

}  // namespace
}  // namespace radray
