#include <gtest/gtest.h>

#include <radray/window/native_window.h>

using namespace radray;

static NativeWindowCreateDescriptor MakeDefaultDesc() {
#if defined(RADRAY_PLATFORM_WINDOWS)
    return Win32WindowCreateDescriptor{
        "Test Native Window",
        800, 600,
        -1, -1,
        true, false, false};
#elif defined(RADRAY_PLATFORM_MACOS)
    return CocoaWindowCreateDescriptor{
        "Test Native Window",
        800, 600,
        -1, -1,
        true, false, false};
#else
    return {};
#endif
}

class NativeWindowTest : public testing::Test {
protected:
    unique_ptr<NativeWindow> window;

    void SetUp() override {
        auto result = CreateNativeWindow(MakeDefaultDesc());
        ASSERT_TRUE(result.HasValue());
        window = std::move(result).Unwrap();
        ASSERT_NE(window, nullptr);
    }

    void TearDown() override {
        if (window) {
            window->Destroy();
        }
    }
};

TEST_F(NativeWindowTest, IsValidAfterCreation) {
    EXPECT_TRUE(window->IsValid());
}

TEST_F(NativeWindowTest, ShouldCloseIsFalseInitially) {
    EXPECT_FALSE(window->ShouldClose());
}

TEST_F(NativeWindowTest, GetSizeReturnsPositive) {
    auto size = window->GetSize();
    EXPECT_GT(size.X, 0);
    EXPECT_GT(size.Y, 0);
}

TEST_F(NativeWindowTest, GetNativeHandlerIsValid) {
    auto handler = window->GetNativeHandler();
    EXPECT_NE(handler.Type, WindowHandlerTag::UNKNOWN);
    EXPECT_NE(handler.Handle, nullptr);
#if defined(RADRAY_PLATFORM_WINDOWS)
    EXPECT_EQ(handler.Type, WindowHandlerTag::HWND);
#elif defined(RADRAY_PLATFORM_MACOS)
    EXPECT_EQ(handler.Type, WindowHandlerTag::NS_VIEW);
#endif
}

TEST_F(NativeWindowTest, IsNotMinimizedAfterCreation) {
    EXPECT_FALSE(window->IsMinimized());
}

TEST_F(NativeWindowTest, DestroyMakesInvalid) {
    window->Destroy();
    EXPECT_FALSE(window->IsValid());
}

TEST_F(NativeWindowTest, EventSignalsAreAccessible) {
    auto& resized = window->EventResized();
    auto& resizing = window->EventResizing();
    auto& touch = window->EventTouch();
    auto& keyboard = window->EventKeyboard();
    auto& mouseWheel = window->EventMouseWheel();
    (void)resized;
    (void)resizing;
    (void)touch;
    (void)keyboard;
    (void)mouseWheel;
}

TEST_F(NativeWindowTest, DispatchEventsDoesNotCrash) {
    window->DispatchEvents();
    EXPECT_TRUE(window->IsValid());
}
