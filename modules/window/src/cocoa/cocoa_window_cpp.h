#pragma once

#include <radray/window/native_window.h>

namespace radray {

Nullable<unique_ptr<NativeWindow>> CreateCocoaWindow(const CocoaWindowCreateDescriptor& desc) noexcept;

}
