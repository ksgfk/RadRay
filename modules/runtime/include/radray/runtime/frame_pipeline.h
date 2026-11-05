#pragma once

#include <radray/runtime/frame_slot.h>
#include <radray/runtime/scene.h>

namespace radray {

// See docs/architecture/runtime.md for the frame loop and ownership contract.
class FramePipeline {
public:
    virtual ~FramePipeline() noexcept;

    // Registrations and processors remain borrowed for the synchronous cache update.
    virtual std::span<const MeshPassRegistration> GetMeshPassRegistrations() noexcept { return {}; }

    // Callbacks are serialized and must not re-enter the scheduler. Extract must not
    // submit asynchronous work. Source is borrowed only through the Extract argument.
    // Snapshot commands and retain data in slot for Record and any later use, including Wait.
    // Use slot.GetArena() for frame-lifetime objects and slot.RetireResource() for retired owners.
    // Frame errors are fatal; report diagnostics and abort instead of returning incomplete work.
    virtual void Extract(const Scene& source, FrameSlot& slot) noexcept = 0;

    // Prepare data, acquire buffers from slot, Begin/End and submit on slot.GetQueue().
    // Source is no longer borrowed. Use only extracted frame data.
    // Frame resource owners may be moved into slot.GetArena() to retain them through Wait.
    // Close all encoders and recordings before returning. Return requires a subsequent Wait,
    // including empty frames. Frame errors are fatal, with no rollback or retry protocol.
    virtual void Record(FrameSlot& slot) noexcept = 0;

    // Called after Record. Return only when all slot submissions are complete
    // and its command storage, CPU data and arena objects can be safely reset or destroyed.
    // Arena objects remain alive throughout Wait; only the scheduler triggers reclamation.
    virtual void Wait(FrameSlot& slot) noexcept = 0;
};

}  // namespace radray
