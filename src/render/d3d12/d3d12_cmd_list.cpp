#include "d3d12_cmd_list.h"

namespace radray::render::d3d12 {

void CmdListD3D12::Destroy() noexcept {
    _cmdList = nullptr;
}

}  // namespace radray::render::d3d12
