#include "external_resource_ipc.h"

#include <limits>
#include "external_resource_test_utils.h"
#include "external_texture_test_utils.h"
#include "known_gpu_issues.h"

#if defined(RADRAY_PLATFORM_WINDOWS)
#include <radray/platform/win32_headers.h>
#include <radray/text_encoding.h>
#include <filesystem>
#include <cstdio>

namespace radray::render::test {
namespace {
enum class Op : uint32_t { Hello,
                           Create,
                           Import,
                           Turn,
                           Block,
                           Stop };
struct Packet {
    vector<uint8_t> Bytes;
    size_t Cursor{0};
    bool Valid{true};
    void Put(uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) Bytes.push_back(uint8_t(value >> (8 * i)));
    }
    uint64_t Get() {
        if (Cursor > Bytes.size() || Bytes.size() - Cursor < 8) {
            Valid = false;
            return 0;
        }
        uint64_t result = 0;
        for (unsigned i = 0; i < 8; ++i) result |= uint64_t(Bytes[Cursor++]) << (8 * i);
        return result;
    }
    bool Done() const { return Valid && Cursor == Bytes.size(); }
};
void PutIdentity(Packet& p, const ExternalDeviceIdentity& i) {
    for (auto b : i.Luid) p.Put(b);
    p.Put(i.NodeMask);
}
ExternalDeviceIdentity GetIdentity(Packet& p) {
    ExternalDeviceIdentity i;
    for (auto& b : i.Luid) b = uint8_t(p.Get());
    i.NodeMask = uint32_t(p.Get());
    return i;
}
void PutEnvironment(Packet& p, const GpuTestEnvironment& environment) {
    p.Put(uint32_t(environment.Backend));
    p.Put(environment.VendorId);
    p.Put(environment.DeviceId);
    p.Put(environment.DriverVersion);
}
GpuTestEnvironment GetEnvironment(Packet& p) { return {RenderBackend(p.Get()), uint32_t(p.Get()), uint32_t(p.Get()), p.Get()}; }
void PutAllocation(Packet& p, const ExternalAllocationInfo& a) {
    p.Put(a.Size);
    p.Put(a.Offset);
    p.Put(a.Dedicated);
}
ExternalAllocationInfo GetAllocation(Packet& p) { return {p.Get(), p.Get(), p.Get() == 1}; }
void PutVulkanIdentity(Packet& p, const std::optional<ExternalVulkanIdentity>& identity) {
    p.Put(identity.has_value());
    if (identity) {
        for (auto b : identity->DeviceUuid) p.Put(b);
        for (auto b : identity->DriverUuid) p.Put(b);
    }
}
std::optional<ExternalVulkanIdentity> GetVulkanIdentity(Packet& p) {
    const auto present = p.Get();
    if (present > 1) p.Valid = false;
    if (present != 1) return std::nullopt;
    ExternalVulkanIdentity i;
    for (auto& b : i.DeviceUuid) b = uint8_t(p.Get());
    for (auto& b : i.DriverUuid) b = uint8_t(p.Get());
    return i;
}
void PutVulkanMemory(Packet& p, const std::optional<ExternalVulkanMemoryInfo>& memory) {
    PutVulkanIdentity(p, memory ? std::optional{memory->Device} : std::nullopt);
    if (memory) p.Put(memory->MemoryTypeIndex);
}
std::optional<ExternalVulkanMemoryInfo> GetVulkanMemory(Packet& p) {
    const auto identity = GetVulkanIdentity(p);
    if (!identity) return std::nullopt;
    return ExternalVulkanMemoryInfo{*identity, uint32_t(p.Get())};
}
void PutBufferInfo(Packet& p, const ExternalBufferInfo& i) {
    p.Put(uint32_t(i.HandleType));
    PutIdentity(p, i.Device);
    p.Put(uint32_t(i.SourceBackend));
    p.Put(i.Descriptor.Size);
    p.Put(uint32_t(i.Descriptor.Memory));
    p.Put(i.Descriptor.Usage.value());
    p.Put(i.Descriptor.Hints.value());
    p.Put(i.NativeSize);
    p.Put(i.NativeFlags);
    p.Put(i.NativeUsage);
    PutAllocation(p, i.Allocation);
    PutVulkanMemory(p, i.VulkanMemory);
}
ExternalBufferInfo GetBufferInfo(Packet& p) {
    ExternalBufferInfo i;
    i.HandleType = ExternalMemoryHandleType(p.Get());
    i.Device = GetIdentity(p);
    i.SourceBackend = RenderBackend(p.Get());
    i.Descriptor.Size = p.Get();
    i.Descriptor.Memory = MemoryType(p.Get());
    i.Descriptor.Usage = BufferUse(p.Get());
    i.Descriptor.Hints = ResourceHint(p.Get());
    i.NativeSize = p.Get();
    i.NativeFlags = uint32_t(p.Get());
    i.NativeUsage = uint32_t(p.Get());
    i.Allocation = GetAllocation(p);
    i.VulkanMemory = GetVulkanMemory(p);
    return i;
}
void PutBuffer(Packet& p, const ExternalBufferExport& e) {
    PutBufferInfo(p, e.Info);
    p.Put(e.Handle.Borrow().Value);
}
ExternalBufferExport GetBuffer(Packet& p) {
    auto info = GetBufferInfo(p);
    return {std::move(info), OwnedExternalHandle{uintptr_t(p.Get())}};
}
void PutTextureDescriptor(Packet& p, const TextureDescriptor& d) {
    p.Put(uint32_t(d.Dim));
    p.Put(d.Width);
    p.Put(d.Height);
    p.Put(d.DepthOrArraySize);
    p.Put(d.MipLevels);
    p.Put(d.SampleCount);
    p.Put(uint32_t(d.Format));
    p.Put(uint32_t(d.Memory));
    p.Put(d.Usage.value());
    p.Put(d.Hints.value());
}
TextureDescriptor GetTextureDescriptor(Packet& p) {
    TextureDescriptor d;
    d.Dim = TextureDimension(p.Get());
    d.Width = uint32_t(p.Get());
    d.Height = uint32_t(p.Get());
    d.DepthOrArraySize = uint32_t(p.Get());
    d.MipLevels = uint32_t(p.Get());
    d.SampleCount = uint32_t(p.Get());
    d.Format = TextureFormat(p.Get());
    d.Memory = MemoryType(p.Get());
    d.Usage = TextureUse(p.Get());
    d.Hints = ResourceHint(p.Get());
    return d;
}
void PutTextureInfo(Packet& p, const ExternalTextureInfo& i) {
    const auto& d = i.Descriptor;
    p.Put(uint32_t(i.HandleType));
    PutIdentity(p, i.Device);
    p.Put(uint32_t(i.SourceBackend));
    PutTextureDescriptor(p, d);
    p.Put(i.NativeFormat);
    p.Put(i.NativeFlags);
    p.Put(i.NativeUsage);
    p.Put(i.NativeTiling);
    PutAllocation(p, i.Allocation);
    PutVulkanMemory(p, i.VulkanMemory);
    p.Put(i.AllowFormatReinterpretation);
}
ExternalTextureInfo GetTextureInfo(Packet& p) {
    ExternalTextureInfo i;
    auto& d = i.Descriptor;
    i.HandleType = ExternalMemoryHandleType(p.Get());
    i.Device = GetIdentity(p);
    i.SourceBackend = RenderBackend(p.Get());
    d = GetTextureDescriptor(p);
    i.NativeFormat = uint32_t(p.Get());
    i.NativeFlags = uint32_t(p.Get());
    i.NativeUsage = uint32_t(p.Get());
    i.NativeTiling = uint32_t(p.Get());
    i.Allocation = GetAllocation(p);
    i.VulkanMemory = GetVulkanMemory(p);
    i.AllowFormatReinterpretation = p.Get() != 0;
    return i;
}
void PutTexture(Packet& p, const ExternalTextureExport& e) {
    PutTextureInfo(p, e.Info);
    p.Put(e.Handle.Borrow().Value);
}
ExternalTextureExport GetTexture(Packet& p) {
    auto info = GetTextureInfo(p);
    return {std::move(info), OwnedExternalHandle{uintptr_t(p.Get())}};
}
void PutFenceInfo(Packet& p, const ExternalFenceInfo& i) {
    p.Put(uint32_t(i.HandleType));
    PutIdentity(p, i.Device);
    PutVulkanIdentity(p, i.VulkanIdentity);
}
ExternalFenceInfo GetFenceInfo(Packet& p) {
    ExternalFenceInfo i;
    i.HandleType = ExternalFenceHandleType(p.Get());
    i.Device = GetIdentity(p);
    i.VulkanIdentity = GetVulkanIdentity(p);
    return i;
}
void PutFence(Packet& p, const ExternalFenceExport& e) {
    PutFenceInfo(p, e.Info);
    p.Put(e.Handle.Borrow().Value);
}
ExternalFenceExport GetFence(Packet& p) {
    auto info = GetFenceInfo(p);
    return {std::move(info), OwnedExternalHandle{uintptr_t(p.Get())}};
}
HANDLE Native(const OwnedExternalHandle& h) { return reinterpret_cast<HANDLE>(h.Borrow().Value); }
OwnedExternalHandle Own(HANDLE h) { return OwnedExternalHandle{reinterpret_cast<uintptr_t>(h)}; }
bool Transfer(HANDLE pipe, void* data, DWORD size, bool write, bool asynchronous) {
    auto* cursor = static_cast<uint8_t*>(data);
    while (size) {
        DWORD count = 0;
        OwnedExternalHandle event;
        OVERLAPPED operation{};
        if (asynchronous) {
            event = Own(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!event.IsValid()) return false;
            operation.hEvent = Native(event);
        }
        const BOOL success = write ? WriteFile(pipe, cursor, size, &count, asynchronous ? &operation : nullptr)
                                   : ReadFile(pipe, cursor, size, &count, asynchronous ? &operation : nullptr);
        if (!success) {
            if (!asynchronous || GetLastError() != ERROR_IO_PENDING) return false;
            if (WaitForSingleObject(Native(event), 30000) != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &operation);
                GetOverlappedResult(pipe, &operation, &count, TRUE);  // Drain canceled pipe I/O, never a GPU wait.
                return false;
            }
            if (!GetOverlappedResult(pipe, &operation, &count, FALSE)) return false;
        }
        if (!count || count > size) return false;
        cursor += count;
        size -= count;
    }
    return true;
}
bool Send(HANDLE pipe, Packet& packet, bool async) {
    Packet length;
    length.Put(packet.Bytes.size());
    return Transfer(pipe, length.Bytes.data(), 8, true, async) && Transfer(pipe, packet.Bytes.data(), DWORD(packet.Bytes.size()), true, async);
}
bool Receive(HANDLE pipe, Packet& packet, bool async) {
    Packet length;
    length.Bytes.resize(8);
    if (!Transfer(pipe, length.Bytes.data(), 8, false, async)) return false;
    const auto size = length.Get();
    if (!size || size > 1024 * 1024 || size % 8) return false;
    packet = {};
    packet.Bytes.resize(size_t(size));
    return Transfer(pipe, packet.Bytes.data(), DWORD(size), false, async);
}
Packet Request(Op op) {
    Packet p;
    p.Put(5);
    p.Put(uint32_t(op));
    return p;
}
bool Reply(HANDLE pipe, uint64_t status) {
    Packet p;
    p.Put(status);
    return Send(pipe, p, false);
}

struct Child {
    OwnedExternalHandle Pipe, Process;
    ~Child() {
        // This supervisor owns the process. A timed-out worker may still reference every slot.
        if (Process.IsValid() && WaitForSingleObject(Native(Process), 0) == WAIT_TIMEOUT) {
            TerminateProcess(Native(Process), 125);
            WaitForSingleObject(Native(Process), 5000);
        }
    }
};
bool StartChild(Child& child, HANDLE job, std::string_view executable, RenderBackend backend, uint32_t index) {
    const auto pipeName = fmt::format("\\\\.\\pipe\\radray-external-{}-{}-{}", GetCurrentProcessId(), GetTickCount64(), index);
    const auto pipeNameW = ToWideChar(pipeName);
    const auto logPath = ToWideChar(fmt::format("interop-worker-{}-{}.log", GetCurrentProcessId(), index));
    const auto executableW = std::filesystem::path{executable}.wstring();
    const auto executableUtf8 = ToMultiByte(executableW);
    if (!pipeNameW || !logPath || !executableUtf8) return false;
    auto command = ToWideChar(fmt::format("\"{}\" --worker \"{}\" {}", *executableUtf8, pipeName, uint32_t(backend)));
    if (!command) return false;
    child.Pipe = Own(CreateNamedPipeW(pipeNameW->c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr));
    if (!child.Pipe.IsValid()) return false;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    auto log = Own(CreateFileW(logPath->c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!log.IsValid()) return false;
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    vector<byte> attributeStorage(attributeSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize)) return false;
    HANDLE inherited[]{Native(log)};
    const bool updated = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr) != FALSE;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = Native(log);
    startup.StartupInfo.hStdInput = Native(log);
    PROCESS_INFORMATION process{};
    const bool created = updated && CreateProcessW(executableW.c_str(), command->data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                                                   nullptr, nullptr, &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(attributes);
    if (!created) return false;
    child.Process = Own(process.hProcess);
    auto thread = Own(process.hThread);
    if (!AssignProcessToJobObject(job, process.hProcess)) return false;
    if (ResumeThread(process.hThread) == DWORD(-1)) return false;
    auto event = Own(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    OVERLAPPED connect{};
    connect.hEvent = Native(event);
    if (!ConnectNamedPipe(Native(child.Pipe), &connect)) {
        const auto error = GetLastError();
        if (error != ERROR_PIPE_CONNECTED) {
            if (error != ERROR_IO_PENDING) return false;
            DWORD count = 0;
            if (WaitForSingleObject(Native(event), 30000) != WAIT_OBJECT_0) {
                CancelIoEx(Native(child.Pipe), &connect);
                GetOverlappedResult(Native(child.Pipe), &connect, &count, TRUE);
                return false;
            }
            if (!GetOverlappedResult(Native(child.Pipe), &connect, &count, FALSE)) return false;
        }
    }
    return true;
}
bool Exchange(Child& child, Packet request, Packet& response) { return Send(Native(child.Pipe), request, true) && Receive(Native(child.Pipe), response, true); }
bool Acknowledge(Child& child, Packet request) {
    Packet response;
    return Exchange(child, std::move(request), response) && response.Get() == 0 && response.Done();
}
// Values in the producer packet are valid only in that producer. Never wrap them as local owned handles.
bool ForwardHandle(Packet& from, Packet& to, Child& producer, Child& consumer) {
    HANDLE target = nullptr;
    const auto value = from.Get();
    if (!from.Valid || !DuplicateHandle(Native(producer.Process), reinterpret_cast<HANDLE>(uintptr_t(value)), Native(consumer.Process), &target, 0, FALSE, DUPLICATE_SAME_ACCESS)) return false;
    to.Put(reinterpret_cast<uintptr_t>(target));
    return true;
}
template <class Info>
bool ForwardRecord(Packet& from, Packet& to, Info (*decode)(Packet&), void (*encode)(Packet&, const Info&), Child& producer, Child& consumer) {
    const auto info = decode(from);
    if (!from.Valid) return false;
    encode(to, info);
    return ForwardHandle(from, to, producer, consumer);
}
bool Turn(Child& worker, uint32_t generation, uint32_t slot, uint32_t expected, uint32_t written, bool first, uint64_t wait, uint64_t signal, uint32_t delay) {
    auto p = Request(Op::Turn);
    p.Put(generation);
    p.Put(slot);
    p.Put(expected);
    p.Put(written);
    p.Put(first);
    p.Put(wait);
    p.Put(signal);
    p.Put(delay);
    return Acknowledge(worker, std::move(p));
}
int Worker(HANDLE pipe, RenderBackend backend) {
    DeviceContext context;
    if (!TryCreateDevice(backend, context, true, true, {1, 0, 0}, nullptr, true, true, nullptr, true)) {
        Reply(pipe, SetupMustFail(context.Status, RequiredBackend(backend)) ? 1 : 77);
        return 0;
    }
    auto extension = context.Device->GetExternalResourceExtension();
    if (!extension) {
        Reply(pipe, 77);
        return 0;
    }
    const auto identity = extension.Get()->GetDeviceIdentity();
    if (const auto* error = std::get_if<ExternalError>(&identity)) {
        std::fprintf(stderr, "External device identity unavailable: %s\n", error->Message.c_str());
        return Reply(pipe, ExternalSupportQuerySucceeded({false, false, *error}) ? 77 : 1) ? 0 : 1;
    }
    GpuTestEnvironment environment;
    if (!TakeExternal(GetGpuTestEnvironment(context), environment)) return Reply(pipe, 1) ? 0 : 1;
    Packet hello;
    hello.Put(0);
    hello.Put(5);
    PutIdentity(hello, std::get<ExternalDeviceIdentity>(identity));
    PutEnvironment(hello, environment);
    if (!Send(pipe, hello, false)) return 1;
    vector<ExternalTestSlot> slots;
    vector<ExternalTestExports> exports;
    unique_ptr<Fence> ready, released;
    ExternalFenceExport readyExport, releasedExport;
    uint32_t generation = std::numeric_limits<uint32_t>::max();
    bool producer = false;
    auto use = ExternalTextureTestUse::Copy;
    Packet p;
    while (Receive(pipe, p, false)) {
        if (p.Get() != 5) return 1;
        const auto op = Op(p.Get());
        if (op == Op::Stop) {
            if (!p.Done()) return 1;
            context.Queue->Wait();
            exports.clear();
            slots.clear();
            ready.reset();
            released.reset();
            readyExport = {};
            releasedExport = {};
            context.Reset();
            return Reply(pipe, testing::UnitTest::GetInstance()->ad_hoc_test_result().Failed() ? 1 : 0) ? 0 : 1;
        }
        if (op == Op::Create || op == Op::Import) {
            const uint32_t next = uint32_t(p.Get()), count = uint32_t(p.Get());
            if (count < 1 || count > 3 || (generation != std::numeric_limits<uint32_t>::max() && next != generation + 1)) return 1;
            context.Queue->Wait();
            exports.clear();
            slots.clear();
            ready.reset();
            released.reset();
            readyExport = {};
            releasedExport = {};
            generation = next;
            slots.resize(count);
            producer = op == Op::Create;
            if (producer) {
                const auto t = GetTextureDescriptor(p);
                const bool reinterpret = p.Get() != 0;
                use = ExternalTextureTestUse(p.Get());
                const ExternalResourceDescriptor memory{ExternalMemoryHandleType(p.Get())};
                const ExternalFenceDescriptor fence{ExternalFenceHandleType(p.Get())};
                if (!p.Done() || uint32_t(use) > 3) return 1;
                const auto b = ExternalTestBufferDescriptor(generation);
                const ExternalTextureDescriptor textureOptions{memory.HandleType, reinterpret};
                const auto bufferSupport = extension.Get()->QueryBufferSupport(b, memory);
                const auto textureSupport = QueryExternalTextureSupport(context, t, textureOptions, fmt::format("ipc-producer-generation-{}", generation));
                const auto fenceSupport = extension.Get()->QueryFenceSupport(fence.HandleType);
                for (const auto& support : {bufferSupport, textureSupport, fenceSupport}) {
                    const auto result = ExternalSupportQuerySucceeded(support);
                    if (!result) {
                        RADRAY_ERR_LOG("{}", result.message());
                        return 1;
                    }
                }
                if (!bufferSupport.CanCreateExportable || !textureSupport.CanCreateExportable || !fenceSupport.CanCreateExportable) {
                    // Exercise the failing API, as well as reporting the unavailable success path.
                    if (!bufferSupport.CanCreateExportable) {
                        const auto rejected = extension.Get()->CreateExportableBuffer(b, memory);
                        if (!std::holds_alternative<ExternalError>(rejected)) return 1;
                    }
                    if (!Reply(pipe, 77)) return 1;
                    continue;
                }
                if (!TakeExternal(extension.Get()->CreateExportableFence(fence), ready) || !TakeExternal(extension.Get()->CreateExportableFence(fence), released) ||
                    !TakeExternal(extension.Get()->ExportFence(ready.get()), readyExport) || !TakeExternal(extension.Get()->ExportFence(released.get()), releasedExport)) return 1;
                exports.resize(count);
                Packet response;
                response.Put(0);
                response.Put(generation);
                response.Put(count);
                response.Put(uint32_t(use));
                PutFence(response, readyExport);
                PutFence(response, releasedExport);
                for (uint32_t i = 0; i < count; ++i) {
                    if (!TakeExternal(extension.Get()->CreateExportableBuffer(b, memory), slots[i].BufferObject) || !TakeExternal(extension.Get()->CreateExportableTexture(t, textureOptions), slots[i].TextureObject) || !ExportExternalTestSlot(*extension.Get(), slots[i], exports[i])) return 1;
                    PutBuffer(response, exports[i].BufferObject);
                    PutTexture(response, exports[i].TextureObject);
                }
                if (!Send(pipe, response, false)) return 1;
            } else {
                use = ExternalTextureTestUse(p.Get());
                const auto sourceEnvironment = GetEnvironment(p);
                auto readyHandle = GetFence(p), releasedHandle = GetFence(p);
                vector<ExternalTestExports> incoming(count);
                for (auto& e : incoming) {
                    e.BufferObject = GetBuffer(p);
                    e.TextureObject = GetTexture(p);
                }
                if (!p.Done() || uint32_t(use) > 3) return 1;
                const auto fenceSupport = extension.Get()->QueryFenceSupport(readyHandle.Info.HandleType);
                if (const auto result = ExternalSupportQuerySucceeded(fenceSupport); !result) {
                    RADRAY_ERR_LOG("{}", result.message());
                    return 1;
                }
                bool supported = fenceSupport.CanImport;
                auto issue = KnownGpuIssue::None;
                for (const auto& e : incoming) {
                    const auto bufferSupport = extension.Get()->QueryBufferSupport(e.BufferObject.Info.Descriptor, {e.BufferObject.Info.HandleType});
                    const auto textureSupport = QueryExternalTextureSupport(context, e.TextureObject.Info.Descriptor, {e.TextureObject.Info.HandleType, e.TextureObject.Info.AllowFormatReinterpretation}, fmt::format("ipc-consumer-generation-{}", generation));
                    for (const auto& support : {bufferSupport, textureSupport}) {
                        const auto result = ExternalSupportQuerySucceeded(support);
                        if (!result) {
                            RADRAY_ERR_LOG("{}", result.message());
                            return 1;
                        }
                    }
                    supported = supported && bufferSupport.CanImport && textureSupport.CanImport;
                    // A successful Create response requires successful source queries and export.
                    const auto known = FindExternalTextureKnownIssue(sourceEnvironment, environment, e.TextureObject.Info.Descriptor,
                                                                     {e.TextureObject.Info.HandleType, e.TextureObject.Info.AllowFormatReinterpretation}, use, {true, false, {}}, textureSupport);
                    if (known != KnownGpuIssue::None) issue = known;
                }
                if (!supported) {
                    if (!Reply(pipe, 77)) return 1;
                    continue;
                }
                KnownGpuIssueSkips skips;
                if (skips.Add(issue, fmt::format("IPC generation={}", generation))) {
                    Packet response;
                    response.Put(78);
                    response.Put(uint32_t(issue));
                    if (!Send(pipe, response, false)) return 1;
                    continue;
                }
                if (!TakeExternal(extension.Get()->ImportFence(readyHandle.Handle.Borrow(), readyHandle.Info), ready) ||
                    !TakeExternal(extension.Get()->ImportFence(releasedHandle.Handle.Borrow(), releasedHandle.Info), released)) return 1;
                for (uint32_t i = 0; i < count; ++i)
                    if (!ImportExternalTestSlot(*extension.Get(), incoming[i], slots[i])) return 1;
                if (!Reply(pipe, 0)) return 1;
            }
        } else if (op == Op::Turn) {
            const auto gen = p.Get(), slot = p.Get(), expected = p.Get(), written = p.Get(), first = p.Get(), wait = p.Get(), signal = p.Get(), delay = p.Get();
            if (!p.Done() || gen != generation || slot >= slots.size() || delay > 1000 || first > 1) {
                if (!Reply(pipe, 2)) return 1;
                continue;
            }
            if (delay) Sleep(DWORD(delay));
            const auto& resource = slots[size_t(slot)];
            const bool ok = resource.TextureObject->GetDesc().SampleCount > 1
                                ? ExternalTextureTurn(context, resource.TextureObject.get(), use, uint32_t(expected), uint32_t(written), first != 0,
                                                      producer ? released.get() : ready.get(), wait, producer ? ready.get() : released.get(), signal, resource.BufferObject.get())
                                : ExternalTestTurn(context, slots[size_t(slot)], use, uint32_t(expected), uint32_t(written), first != 0,
                                                   producer ? released.get() : ready.get(), wait, producer ? ready.get() : released.get(), signal);
            if (!Reply(pipe, ok ? 0 : 1)) return 1;
        } else if (op == Op::Block) {
            if (!p.Done() || slots.empty() || !producer) return 1;
            auto allocator = context.Device->CreateCommandAllocator(context.Queue).Unwrap();
            auto command = context.Device->CreateCommandBuffer(allocator.get()).Unwrap();
            command->Begin();
            const ResourceBarrierDescriptor barriers[]{
                BarrierBufferDescriptor{.Target = slots[0].BufferObject.get(), .Before = BufferState::Common, .After = BufferState::CopySource, .ExternalTransfer = ExternalOwnershipTransfer::Acquire},
                BarrierBufferDescriptor{.Target = slots[0].BufferObject.get(), .Before = BufferState::CopySource, .After = BufferState::Common, .ExternalTransfer = ExternalOwnershipTransfer::Release}};
            command->ResourceBarrier(barriers);
            command->End();
            CommandBuffer* commands[]{command.get()};
            Fence* waits[]{released.get()};
            uint64_t value = 1000000;
            context.Queue->Submit({.CmdBuffers = commands, .WaitFences = waits, .WaitValues = {&value, 1}});
            if (!Reply(pipe, 0)) return 1;
            context.Queue->Wait();  // Deliberately cannot drain; only the supervisor may terminate this process.
            return 1;
        } else
            return 1;
    }
    return 1;
}
}  // namespace

int RunExternalSupervisor(std::string_view executable, RenderBackend producer, RenderBackend consumer, uint32_t count, bool interrupt, bool opaque) {
    if (opaque && (producer != RenderBackend::Vulkan || consumer != RenderBackend::Vulkan)) return 1;
    if (count != 1 && count != 3) return 1;
    auto job = Own(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.IsValid() || !SetInformationJobObject(Native(job), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return 1;
    Child source, target;
    if (!StartChild(source, Native(job), executable, producer, 0) || !StartChild(target, Native(job), executable, consumer, 1)) return 1;
    Packet a, b;
    if (!Receive(Native(source.Pipe), a, true) || !Receive(Native(target.Pipe), b, true)) return 1;
    const auto sourceStatus = a.Get(), targetStatus = b.Get();
    if ((sourceStatus && sourceStatus != 77) || (targetStatus && targetStatus != 77)) return 1;
    if (sourceStatus == 77 || targetStatus == 77) return 77;
    if (a.Get() != 5 || b.Get() != 5) return 1;
    const auto sourceId = GetIdentity(a), targetId = GetIdentity(b);
    const auto sourceEnvironment = GetEnvironment(a), targetEnvironment = GetEnvironment(b);
    if (!a.Done() || !b.Done()) return 1;
    if (sourceEnvironment.Backend != producer || targetEnvironment.Backend != consumer) return 1;
    if (sourceId != targetId) return 77;
    const auto stopWorkers = [&] {
        if (!Acknowledge(target, Request(Op::Stop)) || !Acknowledge(source, Request(Op::Stop))) return false;
        if (WaitForSingleObject(Native(target.Process), 5000) != WAIT_OBJECT_0 || WaitForSingleObject(Native(source.Process), 5000) != WAIT_OBJECT_0) return false;
        DWORD sourceExit = 1, targetExit = 1;
        return GetExitCodeProcess(Native(source.Process), &sourceExit) && GetExitCodeProcess(Native(target.Process), &targetExit) && !sourceExit && !targetExit;
    };
    const TextureFormat formats[]{TextureFormat::RGBA8_UNORM, TextureFormat::RGBA16_FLOAT, TextureFormat::BGRA8_UNORM, TextureFormat::R32_FLOAT};
    const auto* sampleSetting = std::getenv("RADRAY_TEST_EXTERNAL_IPC_MSAA");
    const uint32_t samples = sampleSetting && std::string_view{sampleSetting} == "2" ? 2u : sampleSetting && std::string_view{sampleSetting} == "4" ? 4u
                                                                                        : sampleSetting && std::string_view{sampleSetting} == "8"   ? 8u
                                                                                                                                                    : 1u;
    testing::Test::RecordProperty("ipc_msaa_samples", samples);
    if (samples > 1) fmt::print("IPC MSAA diagnostic samples={} (RGBA8/RGBA16, 2D/2DArray generations)\n", samples);
    for (uint32_t generation = 0; generation < std::size(formats); ++generation) {
        auto request = Request(Op::Create);
        request.Put(generation);
        request.Put(count);
        auto texture = ExternalTestTextureDescriptor(formats[generation], ExternalTextureTestUse::Copy, generation);
        texture.Width = 33 + generation * 2;
        texture.Height = 17 + generation * 2;
        texture.MipLevels = generation == 0 ? 1 : 3;
        if (generation == 1) {
            texture.Dim = TextureDimension::Dim2DArray;
            texture.DepthOrArraySize = 3;
        }
        if (generation == 2) {
            texture.Dim = TextureDimension::CubeArray;
            texture.Height = texture.Width;
            texture.DepthOrArraySize = 12;
        }
        if (generation == 3) {
            texture.Dim = TextureDimension::Dim3D;
            texture.DepthOrArraySize = 7;
        }
        if (samples > 1) {
            texture = {generation % 2 ? TextureDimension::Dim2DArray : TextureDimension::Dim2D, 8, 8, generation % 2 ? 2u : 1u, 1, samples,
                       generation < 2 ? TextureFormat::RGBA8_UNORM : TextureFormat::RGBA16_FLOAT, MemoryType::Device,
                       TextureUse::RenderTarget | TextureUse::Resource | TextureUse::CopySource | TextureUse::CopyDestination};
        }
        PutTextureDescriptor(request, texture);
        request.Put(0);
        request.Put(uint32_t(samples > 1 ? ExternalTextureTestUse::RenderTarget : ExternalTextureTestUse::Copy));
        request.Put(uint32_t(opaque ? ExternalMemoryHandleType::OpaqueWin32 : ExternalMemoryHandleType::D3D12Resource));
        request.Put(uint32_t(opaque ? ExternalFenceHandleType::OpaqueWin32 : ExternalFenceHandleType::D3D12Fence));
        if (!Exchange(source, std::move(request), a)) return 1;
        const auto status = a.Get();
        if (status == 77) return 77;
        if (status) return 1;
        auto imported = Request(Op::Import);
        imported.Put(a.Get());
        imported.Put(a.Get());
        imported.Put(a.Get());
        PutEnvironment(imported, sourceEnvironment);
        if (!ForwardRecord(a, imported, GetFenceInfo, PutFenceInfo, source, target) || !ForwardRecord(a, imported, GetFenceInfo, PutFenceInfo, source, target)) return 1;
        for (uint32_t slot = 0; slot < count; ++slot)
            if (!ForwardRecord(a, imported, GetBufferInfo, PutBufferInfo, source, target) || !ForwardRecord(a, imported, GetTextureInfo, PutTextureInfo, source, target)) return 1;
        if (!a.Done() || !Exchange(target, std::move(imported), b)) return 1;
        const auto importedStatus = b.Get();
        if (importedStatus == 77) return 77;
        if (importedStatus == 78) {
            const auto issue = KnownGpuIssue(b.Get());
            if (!b.Done() || (issue != KnownGpuIssue::PartialMipArray && issue != KnownGpuIssue::SharedMsaa && issue != KnownGpuIssue::CopyOnlyD32S8)) return 1;
            KnownGpuIssueSkips skips;
            if (!skips.Add(issue, fmt::format("IPC generation={} format={} dim={} {}x{} layers={} mips={} samples={} usage={}", generation, texture.Format, texture.Dim, texture.Width, texture.Height, texture.DepthOrArraySize, texture.MipLevels, texture.SampleCount, texture.Usage))) return 1;
            testing::Test::RecordProperty("ipc_completed_generations", generation);
            testing::Test::RecordProperty("ipc_known_issue_generation", generation);
            testing::Test::RecordProperty("ipc_unexecuted_later_generations", std::size(formats) - generation - 1);
            return stopWorkers() ? 78 : 1;
        }
        if (importedStatus || !b.Done()) return 1;
        // An old generation must be rejected before any GPU command is recorded.
        auto stale = Request(Op::Turn);
        stale.Put(std::numeric_limits<uint32_t>::max());
        stale.Put(0);
        stale.Put(0);
        stale.Put(0);
        stale.Put(0);
        stale.Put(0);
        stale.Put(0);
        stale.Put(0);
        if (!Exchange(target, std::move(stale), b) || b.Get() != 2 || !b.Done()) return 1;
        uint64_t timeline = 0;
        for (uint32_t slot = 0; slot < count; ++slot)
            if (!Turn(source, generation, slot, 0, 1 + slot * 100 + generation * 1000, true, 0, ++timeline, 0)) return 1;
        for (uint32_t frame = 0; frame < 4; ++frame) {
            for (uint32_t slot = 0; slot < count; ++slot) {
                const auto seed = 1 + frame * 2 + slot * 100 + generation * 1000;
                const uint64_t value = frame * count + slot + 1;
                if (!Turn(target, generation, slot, seed, seed + 1, false, value, value, slot == 0 ? 40 : 0)) return 1;
            }
            if (interrupt) {
                if (!Acknowledge(source, Request(Op::Block))) {
                    std::fprintf(stderr, "Block submission was not acknowledged\n");
                    return 1;
                }
                if (WaitForSingleObject(Native(source.Process), 100) != WAIT_TIMEOUT) {
                    std::fprintf(stderr, "Producer did not retain pending GPU work\n");
                    return 1;
                }
                if (!TerminateProcess(Native(target.Process), 19) || WaitForSingleObject(Native(target.Process), 5000) != WAIT_OBJECT_0) return 1;

                // Child destructors terminate the blocked producer; its in-flight generation is never reused.
                return 0;
            }
            for (uint32_t slot = 0; slot < count; ++slot) {
                const auto seed = 2 + frame * 2 + slot * 100 + generation * 1000;
                if (!Turn(source, generation, slot, seed, seed + 1, false, frame * count + slot + 1, ++timeline, 0)) return 1;
            }
        }
    }
    return stopWorkers() ? 0 : 1;
}
int ExternalWorkerMain(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    if (argc == 4 && std::string_view{argv[1]} == "--worker") {
        const auto name = std::filesystem::path{argv[2]}.wstring();
        auto pipe = Own(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        if (!pipe.IsValid()) return 1;
        return Worker(Native(pipe), RenderBackend(std::strtoul(argv[3], nullptr, 10)));
    }
    if ((argc == 5 || (argc == 6 && (std::string_view{argv[5]} == "d3d12-resource" || std::string_view{argv[5]} == "opaque-win32"))) && (std::string_view{argv[1]} == "d3d12" || std::string_view{argv[1]} == "vulkan") &&
        (std::string_view{argv[2]} == "d3d12" || std::string_view{argv[2]} == "vulkan") &&
        (std::string_view{argv[3]} == "1" || std::string_view{argv[3]} == "3") &&
        (std::string_view{argv[4]} == "normal" || std::string_view{argv[4]} == "interrupt")) {
        const auto producer = std::string_view{argv[1]} == "d3d12" ? RenderBackend::D3D12 : RenderBackend::Vulkan;
        const auto consumer = std::string_view{argv[2]} == "d3d12" ? RenderBackend::D3D12 : RenderBackend::Vulkan;
        return RunExternalSupervisor(argv[0], producer, consumer, uint32_t(std::strtoul(argv[3], nullptr, 10)), std::string_view{argv[4]} == "interrupt", argc == 6 && std::string_view{argv[5]} == "opaque-win32");
    }
    std::fprintf(stderr, "Usage: radray_external_worker <d3d12|vulkan> <d3d12|vulkan> <1|3> <normal|interrupt> [d3d12-resource|opaque-win32]\n");
    return 1;
}
}  // namespace radray::render::test

#if defined(RADRAY_EXTERNAL_WORKER_MAIN)
int main(int argc, char** argv) { return radray::render::test::ExternalWorkerMain(argc, argv); }
#endif
#endif
