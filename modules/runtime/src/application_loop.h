#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <semaphore>
#include <thread>
#include <radray/runtime/gpu_system.h>
#include <radray/window/native_window.h>

namespace radray {

class Application;
class WindowManager;

/// Internal GT coordinator. Native dispatch may enter TickModal; application callbacks may not.
class ApplicationLoop {
public:
    explicit ApplicationLoop(Application* app);
    ~ApplicationLoop() noexcept;
    void Run();
    void Drain();
    void Wake() noexcept;
    bool CanFinalize() const noexcept;

private:
    enum class State { Running,
                       MaintenanceCPU,
                       MaintenanceGPU,
                       StoppingCPU,
                       StoppingGPU,
                       Stopped };
    struct FrameData {
        uint64_t Serial{0};
        std::chrono::duration<float> DeltaTime{};
        bool Modal{false};
    };

    void Drive();
    void PumpNativeEvents();
    void TickModal(NativeWindow* window);
    std::optional<uint64_t> PrepareFrame(bool modal);
    bool ExecuteFrame(uint64_t identity, bool modal);
    void RecordFrame(uint64_t index, bool discard);
    void RenderThread();
    void RetireReadyFrames();
    bool AdvanceMaintenance();
    void BeginStop();
    void AdvanceStop();
    void StartDrain();
    bool CollectDrain();
    void Idle(uint64_t observedProgress);
    GpuFenceSignal GetWaitingSignal();
    bool CollectThread(std::thread& thread, const std::atomic_bool& returned);

    Application* _app;
    Nullable<WindowManager*> _windows;
    Nullable<GpuSystem*> _gpu;
    sigslot::scoped_connection _modalConnection;
    vector<FrameData> _frames;
    vector<SubmittedFlight> _submitted;
    std::counting_semaphore<> _ready{0};
    std::thread _renderThread;
    std::thread _drainThread;
    std::atomic_bool _stop{false}, _renderReturned{false}, _drainReturned{false};
    std::atomic<uint64_t> _published{0}, _cpuCompleted{0}, _discardBefore{0}, _progress{0};
    std::mutex _wakeMutex;
    std::condition_variable _wakeCondition;
    State _state{State::Running};
    uint64_t _retireIndex{0}, _operationBoundary{0}, _maintenanceTarget{0}, _lastModalPublished{0};
    std::optional<uint64_t> _prepared;
    std::chrono::steady_clock::time_point _lastFrameTime{};
    uint64_t _waitingSerial{0};
    std::chrono::steady_clock::time_point _gpuDeadline{};
    uint32_t _nativeDepth{0};
    bool _advancing{false}, _modalTick{false}, _driving{false};
    bool _systemsStopping{false}, _drainStarted{false}, _drainFinished{false};
};

}  // namespace radray
