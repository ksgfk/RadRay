#include <gtest/gtest.h>

#include <chrono>

#include <radray/logger.h>
#include <radray/window/win32/win32_window.h>

namespace radray {
namespace {

class NativeEventPumpTest : public testing::Test {
protected:
    void SetUp() override {
        NativeWindow::GlobalInit();
        Pump = NativeEventPump::Create(NativeWindowType::Win32HWND, NativeEventDelivery::Deferred).Release();
        ASSERT_TRUE(Pump);
        Window = NativeWindow::Create(Win32WindowCreateDescriptor{.Title = "RadRay event pump test", .Width = 160, .Height = 120, .StartVisible = false}).Release();
        ASSERT_TRUE(Window);
        ASSERT_TRUE(Pump->Register(Window.get()));
        Hwnd = static_cast<HWND>(Window->GetNativeHandler());
        // Drop creation-time events so each test starts from an empty queue.
        Pump->PumpNativeEvents();
        Pump->DispatchPendingEvents();
    }
    void TearDown() override {
        Window.reset();
        Pump.reset();
        NativeWindow::GlobalShutdown();
    }
    void RecordText() {
        Window->EventTextInput().connect([this](std::string_view text) { Texts.emplace_back(text); });
    }

    unique_ptr<NativeEventPump> Pump;
    unique_ptr<NativeWindow> Window;
    HWND Hwnd{};
    vector<string> Texts;
};

TEST_F(NativeEventPumpTest, CreateRejectsOtherWindowTypes) {
    EXPECT_FALSE(NativeEventPump::Create(NativeWindowType::CocoaNSWindow));
}

TEST_F(NativeEventPumpTest, RegisterRejectsNullWindow) {
    EXPECT_FALSE(Pump->Register(nullptr));
}

TEST_F(NativeEventPumpTest, DeferredEventsWaitForDispatch) {
    RecordText();
    ::SendMessageW(Hwnd, WM_CHAR, 'a', 0);
    EXPECT_TRUE(Texts.empty());
    Pump->DispatchPendingEvents();
    EXPECT_EQ(Texts, (vector<string>{"a"}));
}

TEST_F(NativeEventPumpTest, DispatchPreservesNativeOrder) {
    vector<string> events;
    Window->EventKeyboard().connect([&](KeyCode, Action action) { events.push_back(action == Action::RELEASED ? "key up" : "key down"); });
    Window->EventTextInput().connect([&](std::string_view text) { events.emplace_back(text); });
    Window->EventMouseWheel().connect([&](int delta) { events.emplace_back(radray::format("wheel {}", delta)); });
    Window->EventFocused().connect([&](bool focused) { events.push_back(focused ? "focus" : "blur"); });
    ::SendMessageW(Hwnd, WM_KEYDOWN, VK_F6, 0);
    ::SendMessageW(Hwnd, WM_CHAR, 'a', 0);
    ::SendMessageW(Hwnd, WM_KEYUP, VK_F6, 0);
    ::SendMessageW(Hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, 120), 0);
    ::SendMessageW(Hwnd, WM_KILLFOCUS, 0, 0);
    Pump->DispatchPendingEvents();
    EXPECT_EQ(events, (vector<string>{"key down", "a", "key up", "wheel 120", "blur"}));
}

TEST_F(NativeEventPumpTest, RepeatedKeyDownReportsRepeat) {
    vector<Action> actions;
    Window->EventKeyboard().connect([&](KeyCode, Action action) { actions.push_back(action); });
    ::SendMessageW(Hwnd, WM_KEYDOWN, VK_F6, 0);
    ::SendMessageW(Hwnd, WM_KEYDOWN, VK_F6, LPARAM{1} << 30);
    Pump->DispatchPendingEvents();
    EXPECT_EQ(actions, (vector<Action>{Action::PRESSED, Action::REPEATED}));
}

TEST_F(NativeEventPumpTest, SurrogatePairBecomesOneUtf8Text) {
    RecordText();
    ::SendMessageW(Hwnd, WM_CHAR, 0xD83D, 0);
    ::SendMessageW(Hwnd, WM_CHAR, 0xDE00, 0);
    Pump->DispatchPendingEvents();
    EXPECT_EQ(Texts, (vector<string>{"\xf0\x9f\x98\x80"}));
}

TEST_F(NativeEventPumpTest, EventsRaisedByCallbacksWaitForNextDispatch) {
    RecordText();
    Window->EventTextInput().connect([this](std::string_view text) {
        if (text == "a") ::SendMessageW(Hwnd, WM_CHAR, 'b', 0);
    });
    ::SendMessageW(Hwnd, WM_CHAR, 'a', 0);
    Pump->DispatchPendingEvents();
    EXPECT_EQ(Texts, (vector<string>{"a"}));
    Pump->DispatchPendingEvents();
    EXPECT_EQ(Texts, (vector<string>{"a", "b"}));
}

TEST_F(NativeEventPumpTest, UnregisterDropsPendingEvents) {
    RecordText();
    ::SendMessageW(Hwnd, WM_CHAR, 'a', 0);
    Pump->Unregister(Window.get());
    ASSERT_TRUE(Pump->Register(Window.get()));
    Pump->DispatchPendingEvents();
    EXPECT_TRUE(Texts.empty());
}

TEST_F(NativeEventPumpTest, PumpNativeEventsCollectsAtMostLimit) {
    RecordText();
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(::PostMessageW(Hwnd, WM_CHAR, 'a', 0));
    Pump->PumpNativeEvents(4);
    Pump->DispatchPendingEvents();
    EXPECT_EQ(Texts.size(), 4u);
    Pump->PumpNativeEvents();
    Pump->DispatchPendingEvents();
    EXPECT_EQ(Texts.size(), 10u);
}

TEST_F(NativeEventPumpTest, CloseRequestIsDeliveredOnce) {
    int closes = 0;
    Window->EventCloseRequested().connect([&] { ++closes; });
    ::SendMessageW(Hwnd, WM_CLOSE, 0, 0);
    EXPECT_TRUE(Window->ShouldClose());
    Pump->DispatchPendingEvents();
    Pump->DispatchCloseRequest(Window.get());
    EXPECT_EQ(closes, 1);
}

TEST_F(NativeEventPumpTest, StopEventDeliveryDiscardsPendingAndLaterEvents) {
    RecordText();
    ::SendMessageW(Hwnd, WM_CHAR, 'a', 0);
    Pump->StopEventDelivery();
    ::SendMessageW(Hwnd, WM_CHAR, 'b', 0);
    Pump->DispatchPendingEvents();
    EXPECT_TRUE(Texts.empty());
}

TEST_F(NativeEventPumpTest, DispatchCloseRequestWorksAfterStop) {
    int closes = 0;
    Window->EventCloseRequested().connect([&] { ++closes; });
    ::SendMessageW(Hwnd, WM_CLOSE, 0, 0);
    Pump->StopEventDelivery();
    Pump->DispatchCloseRequest(Window.get());
    Pump->DispatchCloseRequest(Window.get());
    EXPECT_EQ(closes, 1);
}

TEST_F(NativeEventPumpTest, WakeEndsWaitEarly) {
    Pump->Wake();
    const auto start = std::chrono::steady_clock::now();
    Pump->WaitEvents(5000);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds{2});
}

TEST_F(NativeEventPumpTest, ImmediatePumpDeliversDuringNativeDispatch) {
    Pump->Unregister(Window.get());
    auto immediate = NativeEventPump::Create(NativeWindowType::Win32HWND).Release();
    ASSERT_TRUE(immediate);
    ASSERT_TRUE(immediate->Register(Window.get()));
    RecordText();
    ::SendMessageW(Hwnd, WM_CHAR, 'a', 0);
    EXPECT_EQ(Texts, (vector<string>{"a"}));
    immediate->Unregister(Window.get());
}

}  // namespace
}  // namespace radray
