#include <gtest/gtest.h>

#include <chrono>
#include <semaphore>
#include <thread>
#include <radray/window/win32/win32_window.h>

namespace radray {
namespace {

class NativeEventPumpTest : public testing::Test {
protected:
    void SetUp() override {
        NativeWindow::GlobalInit();
        pump = NativeEventPump::Create(NativeWindowType::Win32HWND, NativeEventDelivery::Deferred).Unwrap();
        Create();
    }
    void TearDown() override {
        window.reset();
        pump.reset();
        NativeWindow::GlobalShutdown();
    }
    void Create() {
        window = NativeWindow::Create(Win32WindowCreateDescriptor{.Title = "Deferred event test", .Width = 160, .Height = 120, .StartVisible = false}).Unwrap();
        ASSERT_TRUE(pump->Register(window.get()));
        hwnd = static_cast<HWND>(window->GetNativeHandler());
        pump->PumpNativeEvents();
        pump->DispatchPendingEvents();
    }
    unique_ptr<NativeEventPump> pump;
    unique_ptr<NativeWindow> window;
    HWND hwnd{};
};

TEST_F(NativeEventPumpTest, OrderedInputOwnsTextAndFreezesDispatchBoundary) {
    vector<string> events;
    window->EventKeyboard().connect([&](KeyCode, Action action) { events.push_back(fmt::format("key:{}", static_cast<int>(action))); });
    window->EventTextInput().connect([&](std::string_view text) {
        events.emplace_back(text);
        if (text == "a") {
            ::SendMessageW(hwnd, WM_CHAR, 'z', 0);
            pump->DispatchPendingEvents();
        }
    });
    window->EventFocused().connect([&](bool value) { events.push_back(value ? "focus" : "blur"); });
    window->EventCaptureLost().connect([&] { events.push_back("capture"); });
    window->EventMouseWheel().connect([&](int delta) { events.push_back(fmt::format("wheel:{}", delta)); });
    ::SendMessageW(hwnd, WM_KEYDOWN, VK_F6, 0);
    ::SendMessageW(hwnd, WM_KEYDOWN, VK_F6, LPARAM{1} << 30);
    ::SendMessageW(hwnd, WM_KEYUP, VK_F6, 0);
    ::SendMessageW(hwnd, WM_CHAR, 'a', 0);
    ::SendMessageW(hwnd, WM_CHAR, 0xD83D, 0);
    ::SendMessageW(hwnd, WM_CHAR, 0xDE00, 0);
    ::SendMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, 120), 0);
    ::SendMessageW(hwnd, WM_KILLFOCUS, 0, 0);
    ::SendMessageW(hwnd, WM_CAPTURECHANGED, 0, 0);
    EXPECT_TRUE(events.empty());
    pump->DispatchPendingEvents();
    const vector<string> expected{
        fmt::format("key:{}", static_cast<int>(Action::PRESSED)),
        fmt::format("key:{}", static_cast<int>(Action::REPEATED)),
        fmt::format("key:{}", static_cast<int>(Action::RELEASED)),
        "a", "\xf0\x9f\x98\x80", "wheel:120", "blur", "capture"};
    EXPECT_EQ(events, expected);
    pump->DispatchPendingEvents();
    ASSERT_EQ(events.size(), expected.size() + 1);
    EXPECT_EQ(events.back(), "z");
}

TEST_F(NativeEventPumpTest, UnregisterCancelsOldIdentityAndDestructionNotifications) {
    int callbacks = 0;
    window->EventTextInput().connect([&](std::string_view) { ++callbacks; });
    ::SendMessageW(hwnd, WM_CHAR, 'a', 0);
    pump->Unregister(window.get());
    ::SendMessageW(hwnd, WM_CHAR, 'b', 0);
    ASSERT_TRUE(pump->Register(window.get()));
    pump->DispatchPendingEvents();
    EXPECT_EQ(callbacks, 0);
    ::SendMessageW(hwnd, WM_CHAR, 'c', 0);
    window.reset();
    Create();
    window->EventTextInput().connect([&](std::string_view) { ++callbacks; });
    pump->DispatchPendingEvents();
    EXPECT_EQ(callbacks, 0);
}

TEST_F(NativeEventPumpTest, BoundedNativeCollectionAndTerminalClose) {
    int texts = 0, closes = 0;
    window->EventTextInput().connect([&](std::string_view) { ++texts; });
    window->EventCloseRequested().connect([&] { ++closes; });
    for (int i = 0; i < 300; ++i) ASSERT_TRUE(::PostMessageW(hwnd, WM_CHAR, 'a', 0));
    pump->PumpNativeEvents();
    EXPECT_EQ(texts, 0);
    pump->DispatchPendingEvents();
    EXPECT_GT(texts, 0);
    EXPECT_LE(texts, 256);
    pump->PumpNativeEvents();
    pump->DispatchPendingEvents();
    EXPECT_EQ(texts, 300);
    ::SendMessageW(hwnd, WM_CLOSE, 0, 0);
    EXPECT_TRUE(window->ShouldClose());
    EXPECT_EQ(closes, 0);
    pump->StopEventDelivery();
    ::SendMessageW(hwnd, WM_CHAR, 'b', 0);
    pump->DispatchPendingEvents();
    pump->DispatchCloseRequest(window.get());
    pump->DispatchCloseRequest(window.get());
    EXPECT_EQ(closes, 1);
    EXPECT_EQ(texts, 300);
}

TEST_F(NativeEventPumpTest, EarlyAndCoalescedWakeAndActualThreadExit) {
    using Clock = std::chrono::steady_clock;
    pump->Wake();
    pump->Wake();
    auto start = Clock::now();
    pump->WaitEvents(1000);
    EXPECT_LT(Clock::now() - start, std::chrono::milliseconds{500});
    std::thread worker([&] { pump->Wake(); });
    HANDLE handle = worker.native_handle();
    const auto deadline = Clock::now() + std::chrono::seconds{5};
    while (::WaitForSingleObject(handle, 0) != WAIT_OBJECT_0 && Clock::now() < deadline) {
        pump->PumpNativeEvents();
        pump->WaitEvents(10, std::span<void* const>{&handle, 1});
    }
    EXPECT_EQ(::WaitForSingleObject(handle, 0), WAIT_OBJECT_0);
    worker.join();
}

TEST_F(NativeEventPumpTest, IndependentWindowKeepsImmediateCallbacks) {
    pump->Unregister(window.get());
    auto immediate = NativeEventPump::Create(NativeWindowType::Win32HWND).Unwrap();
    ASSERT_TRUE(immediate->Register(window.get()));
    int count = 0;
    window->EventTextInput().connect([&](std::string_view) { ++count; });
    ::SendMessageW(hwnd, WM_CHAR, 'a', 0);
    EXPECT_EQ(count, 1);
    immediate->Unregister(window.get());
}

TEST_F(NativeEventPumpTest, StopUnwindsNativeMenuBeforeOuterDestruction) {
    struct StopContext {
        NativeEventPump* Pump;
        bool Requested{false};
    } context{pump.get()};
    window->Show();
    HMENU menu = ::CreatePopupMenu();
    ASSERT_NE(menu, nullptr);
    EXPECT_TRUE(::AppendMenuW(menu, MF_STRING, 1, L"Frame loop stop test"));
    const auto identity = reinterpret_cast<UINT_PTR>(&context);
    const auto timer = ::SetTimer(hwnd, identity, 20, [](HWND owner, UINT, UINT_PTR id, DWORD) {
        ::KillTimer(owner, id);
        auto* stop = reinterpret_cast<StopContext*>(id);
        stop->Requested = true;
        stop->Pump->StopEventDelivery();
    });
    ASSERT_NE(timer, 0u);
    std::binary_semaphore finished{0};
    std::atomic_bool neededFallback{false};
    std::thread fallback([&] {
        if (!finished.try_acquire_for(std::chrono::seconds{3})) {
            neededFallback = true;
            ::PostMessageW(hwnd, WM_CANCELMODE, 0, 0);
        }
    });
    const auto position = window->GetPosition();
    ::TrackPopupMenu(menu, TPM_RETURNCMD, position.x() + 20, position.y() + 20, 0, hwnd, nullptr);
    finished.release();
    fallback.join();
    ::KillTimer(hwnd, timer);
    ::DestroyMenu(menu);
    EXPECT_TRUE(context.Requested);
    EXPECT_FALSE(neededFallback.load());
    EXPECT_TRUE(window->IsValid());
}

}  // namespace
}  // namespace radray
