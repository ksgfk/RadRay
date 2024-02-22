#pragma once

#include <memory>
#include <string>
#include <radray/types.h>

namespace radray::window {

void GlobalInit() noexcept;
void GlobalPollEvents() noexcept;
void GlobalTerminate() noexcept;

class NativeWindow {
public:
    class Impl {
    public:
        virtual ~Impl() noexcept = default;
    };

    NativeWindow(std::string name, uint32 width, uint32 height, bool resizable = false, bool fullScreen = false) noexcept;
    ~NativeWindow() noexcept;
    NativeWindow(const NativeWindow&) = delete;
    NativeWindow(NativeWindow&&) = default;
    NativeWindow& operator=(NativeWindow&&) noexcept = default;
    NativeWindow& operator=(const NativeWindow&) noexcept = delete;

    bool IsValid() const noexcept;
    bool ShouldClose() const noexcept;
    void Destroy() noexcept;

private:
    std::unique_ptr<Impl> _impl;
};

}  // namespace radray::window
