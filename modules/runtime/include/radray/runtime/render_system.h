#pragma once

#include <cstddef>

#include <compare>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include <radray/nullable.h>
#include <radray/render/gpu_resource.h>
#include <radray/types.h>

namespace radray {

struct FrameId {
    uint64_t Value{0};

    constexpr explicit operator bool() const noexcept { return Value != 0; }

    friend constexpr auto operator<=>(const FrameId&, const FrameId&) noexcept = default;
};

struct RenderFrameStats {
    uint64_t DeferredReleaseCount{0};
    uint64_t TransientBytesAllocated{0};
    uint64_t ConstantBytesAllocated{0};
};

class RenderFrameTransientAllocator {
public:
    struct Descriptor {
        size_t InitialCapacity{64 * 1024};
    };

    RenderFrameTransientAllocator() noexcept = default;
    explicit RenderFrameTransientAllocator(const Descriptor& desc) noexcept;
    RenderFrameTransientAllocator(const RenderFrameTransientAllocator&) = delete;
    RenderFrameTransientAllocator& operator=(const RenderFrameTransientAllocator&) = delete;
    RenderFrameTransientAllocator(RenderFrameTransientAllocator&&) noexcept = default;
    RenderFrameTransientAllocator& operator=(RenderFrameTransientAllocator&&) noexcept = default;
    ~RenderFrameTransientAllocator() noexcept = default;

    void* Allocate(size_t size, size_t alignment = alignof(std::max_align_t));

    void Reset() noexcept;

    void Clear() noexcept;

    size_t GetUsedSize() const noexcept { return _used; }

    size_t GetCapacity() const noexcept { return _storage.size(); }

private:
    void EnsureCapacity(size_t requiredSize);

    Descriptor _desc{};
    vector<byte> _storage{};
    size_t _used{0};
};

struct RenderFrameCompletionHooks {
    std::function<bool()> IsReady{};
    std::function<void()> Wait{};
    std::function<std::exception_ptr()> GetError{};
};

struct RenderServiceDescriptor {
    shared_ptr<render::Device> Device{};
    render::CBufferArena::Descriptor CBufferArena{};
    RenderFrameTransientAllocator::Descriptor TransientAllocator{};
};

class RenderFrame;
class RenderFrameTicket;

namespace detail {

struct RenderServiceState;
struct RenderFrameState;

struct CompletionCallbackHandle {
    uint64_t Id{0};

    constexpr bool IsValid() const noexcept { return Id != 0; }
};

CompletionCallbackHandle RegisterFrameCompletionCallback(
    const shared_ptr<RenderFrameState>& frame,
    std::function<void()> callback) noexcept;

void UnregisterFrameCompletionCallback(
    const weak_ptr<RenderFrameState>& frame,
    CompletionCallbackHandle handle) noexcept;

void ObserveFrameCompletion(const shared_ptr<RenderFrameState>& frame) noexcept;

void WaitForFrameCompletion(const shared_ptr<RenderFrameState>& frame) noexcept;

bool IsFrameComplete(const shared_ptr<RenderFrameState>& frame) noexcept;

std::exception_ptr GetFrameError(const shared_ptr<RenderFrameState>& frame) noexcept;

}  // namespace detail

class RenderService {
public:
    RenderService();
    explicit RenderService(const RenderServiceDescriptor& desc);
    RenderService(const RenderService&) = delete;
    RenderService& operator=(const RenderService&) = delete;
    RenderService(RenderService&& other) noexcept;
    RenderService& operator=(RenderService&& other) noexcept;
    ~RenderService() noexcept;

    RenderFrame BeginFrame();

    void CollectRetiredFrames() noexcept;

    void WaitIdle() noexcept;

    void Shutdown() noexcept;

    Nullable<render::Device*> GetDevice() const noexcept;

private:
    explicit RenderService(shared_ptr<detail::RenderServiceState> state) noexcept;

    shared_ptr<detail::RenderServiceState> _state{};
};

class RenderFrame {
public:
    RenderFrame() noexcept = default;
    RenderFrame(const RenderFrame&) = delete;
    RenderFrame& operator=(const RenderFrame&) = delete;
    RenderFrame(RenderFrame&& other) noexcept;
    RenderFrame& operator=(RenderFrame&& other) noexcept;
    ~RenderFrame() noexcept;

    bool IsValid() const noexcept;

    FrameId GetFrameId() const noexcept;

    Nullable<render::CommandBuffer*> GetCommandBuffer() const noexcept;

    void SetCommandBuffer(Nullable<unique_ptr<render::CommandBuffer>> commandBuffer) noexcept;

    Nullable<const render::CBufferArena*> GetCBufferArena() const noexcept;

    render::CBufferArena::Allocation AllocateConstantBuffer(uint64_t size) noexcept;

    void* AllocateTransient(size_t size, size_t alignment = alignof(std::max_align_t));

    const RenderFrameTransientAllocator& GetTransientAllocator() const noexcept;

    RenderFrameStats& GetStats() noexcept;

    const RenderFrameStats& GetStats() const noexcept;

    void SetCompletionHooks(RenderFrameCompletionHooks hooks);

    void DeferRelease(std::function<void()> callback);

    void Cancel() noexcept;

    RenderFrameTicket Submit() &;

    RenderFrameTicket Submit() &&;

private:
    friend class RenderService;

    explicit RenderFrame(shared_ptr<detail::RenderFrameState> state) noexcept;

    RenderFrameTicket SubmitImpl();

    shared_ptr<detail::RenderFrameState> _state{};
};

class RenderFrameTicket {
public:
    class Sender {
    public:
        using sender_concept = STDEXEC::sender_t;
        using completion_signatures =
            STDEXEC::completion_signatures<
                STDEXEC::set_value_t(),
                STDEXEC::set_error_t(std::exception_ptr)>;

        Sender() noexcept = default;
        explicit Sender(shared_ptr<detail::RenderFrameState> state) noexcept
            : _state(std::move(state)) {}

        template <class Receiver>
        class Operation {
        public:
            using operation_state_concept = STDEXEC::operation_state_t;

            Operation(shared_ptr<detail::RenderFrameState> state, Receiver receiver) noexcept(
                std::is_nothrow_move_constructible_v<shared_ptr<detail::RenderFrameState>> &&
                std::is_nothrow_move_constructible_v<Receiver>)
                : _state(std::move(state)),
                  _receiver(std::move(receiver)) {}

            Operation(const Operation&) = delete;
            Operation& operator=(const Operation&) = delete;
            Operation(Operation&&) = delete;
            Operation& operator=(Operation&&) = delete;

            ~Operation() noexcept = default;

            void start() & noexcept {
                if (!_state) {
                    CompleteInvalid();
                    return;
                }

                detail::WaitForFrameCompletion(_state);
                Complete();
            }

        private:
            [[noreturn]] static void ThrowInvalidState() {
                throw std::logic_error("RenderFrameTicket sender started with invalid frame state");
            }

            void CompleteInvalid() noexcept {
                auto receiver = std::move(_receiver);
                try {
                    ThrowInvalidState();
                } catch (...) {
                    STDEXEC::set_error(std::move(receiver), std::current_exception());
                }
            }

            void Complete() noexcept {
                auto state = _state;
                auto error = detail::GetFrameError(state);

                auto receiver = std::move(_receiver);
                if (error) {
                    STDEXEC::set_error(std::move(receiver), error);
                    return;
                }

                STDEXEC::set_value(std::move(receiver));
            }

            shared_ptr<detail::RenderFrameState> _state{};
            Receiver _receiver;
        };

        template <class Receiver>
        auto connect(Receiver receiver) const
            -> Operation<std::decay_t<Receiver>> {
            return Operation<std::decay_t<Receiver>>{_state, std::move(receiver)};
        }

    private:
        shared_ptr<detail::RenderFrameState> _state{};
    };

    RenderFrameTicket() noexcept = default;

    bool IsValid() const noexcept { return static_cast<bool>(_state); }

    FrameId GetFrameId() const noexcept;

    bool IsComplete() const noexcept;

    bool HasError() const noexcept;

    std::exception_ptr GetError() const noexcept;

    void Wait() const;

    Sender AsSender() const noexcept { return Sender{_state}; }

    template <class Promise>
    auto as_awaitable(Promise& promise) const
        noexcept(noexcept(STDEXEC::as_awaitable(std::declval<Sender>(), promise)))
            -> decltype(STDEXEC::as_awaitable(std::declval<Sender>(), promise)) {
        return STDEXEC::as_awaitable(AsSender(), promise);
    }

private:
    friend class RenderFrame;

    explicit RenderFrameTicket(shared_ptr<detail::RenderFrameState> state) noexcept
        : _state(std::move(state)) {}

    shared_ptr<detail::RenderFrameState> _state{};
};

}  // namespace radray
