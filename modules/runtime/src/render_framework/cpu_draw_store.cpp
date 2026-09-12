#include <radray/runtime/render_framework/cpu_draw_record.h>
#include "render_memory_measure.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

#include <radray/profiler.h>

#include <radray/runtime/render_framework/render_scene_snapshot.h>
#include <radray/runtime/shader_program.h>

namespace radray {

namespace {
std::atomic<uint64_t> gNextEffectiveState{1};
size_t EffectiveStateHash(const MaterialPipelineState& state) noexcept {
    HashCode hash;
    const auto& primitive = state.Primitive;
    hash.Add(static_cast<uint32_t>(primitive.FaceClockwise));
    hash.Add(static_cast<uint32_t>(primitive.Cull));
    hash.Add(static_cast<uint32_t>(primitive.Poly));
    hash.Add(primitive.UnclippedDepth);
    hash.Add(primitive.Conservative);
    const auto& depth = state.DepthStencil;
    hash.Add(static_cast<uint32_t>(depth.DepthCompare));
    hash.Add(depth.DepthBias.Constant);
    hash.Add(depth.DepthBias.SlopScale);
    hash.Add(depth.DepthBias.Clamp);
    hash.Add(depth.DepthTestEnable);
    hash.Add(depth.DepthWriteEnable);
    hash.Add(depth.Stencil.has_value());
    if (depth.Stencil) {
        const auto addFace = [&](const render::StencilFaceState& face) {
            hash.Add(static_cast<uint32_t>(face.Compare));
            hash.Add(static_cast<uint32_t>(face.FailOp));
            hash.Add(static_cast<uint32_t>(face.DepthFailOp));
            hash.Add(static_cast<uint32_t>(face.PassOp));
        };
        addFace(depth.Stencil->Front);
        addFace(depth.Stencil->Back);
        hash.Add(depth.Stencil->ReadMask);
        hash.Add(depth.Stencil->WriteMask);
    }
    hash.Add(state.Blend.has_value());
    if (state.Blend) {
        const auto addBlend = [&](const render::BlendComponent& blend) {
            hash.Add(static_cast<uint32_t>(blend.Src));
            hash.Add(static_cast<uint32_t>(blend.Dst));
            hash.Add(static_cast<uint32_t>(blend.Op));
        };
        addBlend(state.Blend->Color);
        addBlend(state.Blend->Alpha);
    }
    hash.Add(state.WriteMask.value());
    return hash.ToHashCode();
}
}  // namespace

RenderMemoryStats CpuDrawStore::GetMemoryStats() const noexcept {
    RenderMemoryStats result;
    result.ObjectBytes = sizeof(*this);
    result.LiveEntries = _cache.size();
    result.DirtyEntries = _dirtyPolicies.size() + _bindingTouched.size() + _geometryTouched.size() + _syncPrimitives.size();
    detail::MeasureMap(result, _cache);
    for (const auto& [key, cached] : _cache) detail::MeasureString(result, key.PassName);
    detail::MeasureMap(result, _primitiveKeys);
    for (const auto& [id, keys] : _primitiveKeys) {
        detail::MeasureVector(result, keys.Keys, true);
        for (const auto& key : keys.Keys) detail::MeasureString(result, key.PassName);
        result.DependencyEdges += keys.Keys.size();
    }
    detail::MeasureMap(result, _bindings);
    detail::MeasureVector(result, _bindingKeys);
    detail::MeasureVector(result, _bindingData);
    detail::MeasureVector(result, _bindingUsers);
    detail::MeasureVector(result, _freeBindings);
    detail::MeasureVector(result, _bindingChanged);
    detail::MeasureVector(result, _bindingTouched);
    detail::MeasureMap(result, _states);
    detail::MeasureMap(result, _stateBuckets);
    for (const auto& [hash, bucket] : _stateBuckets) detail::MeasureVector(result, bucket, true);
    detail::MeasureMap(result, _geometryIndices);
    detail::MeasureVector(result, _geometryEntries);
    detail::MeasureVector(result, _geometryPlans);
    for (const auto& plan : _geometryPlans) detail::MeasureVector(result, plan.Runs, true);
    detail::MeasureVector(result, _freeGeometry);
    detail::MeasureVector(result, _geometryTouched);
    detail::MeasureVector(result, _geometryChanged);
    detail::MeasureVector(result, _changedGeometryRanges);
    detail::MeasureMap(result, _policyUsers);
    for (const auto& [policy, users] : _policyUsers) {
        detail::MeasureMap(result, users);
        result.DependencyEdges += users.size();
    }
    detail::MeasureVector(result, _policies);
    for (const auto& policy : _policies) detail::MeasureString(result, policy.PassName);
    detail::MeasureVector(result, _dirtyPolicies);
    detail::MeasureVector(result, _changedDrawRanges);
    detail::MeasureVector(result, _changedBindingRanges);
    detail::MeasureVector(result, _syncPrimitives);
    detail::MeasureVector(result, _syncMarked);
    auto layouts = _layouts.GetMemoryStats();
    layouts.ObjectBytes = 0;
    result.Add(layouts);
    return result;
}

uint64_t CpuDrawStore::AcquireState(const MaterialPipelineState& state) {
    const auto hash = EffectiveStateHash(state);
    auto& bucket = _stateBuckets[hash];
    for (auto id : bucket) {
        auto& entry = _states.at(id);
        if (entry.State == state) {
            ++entry.Users;
            return id;
        }
    }
    const auto id = gNextEffectiveState.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) RADRAY_ABORT("Effective state identity exhausted");
    _states.emplace(id, StateEntry{state, 1, hash});
    bucket.push_back(id);
    return id;
}
void CpuDrawStore::ReleaseState(uint64_t id) {
    if (id == 0) return;
    const auto found = _states.find(id);
    if (found == _states.end() || --found->second.Users != 0) return;
    const auto bucket = _stateBuckets.find(found->second.Hash);
    if (bucket != _stateBuckets.end()) {
        std::erase(bucket->second, id);
        if (bucket->second.empty()) _stateBuckets.erase(bucket);
    }
    _states.erase(found);
}
void CpuDrawStore::MarkGeometry(uint32_t index) {
    if (_geometryChanged.size() <= index) _geometryChanged.resize(size_t{index} + 1);
    if (!_geometryChanged[index]) {
        _geometryChanged[index] = 1;
        _geometryTouched.push_back(index);
    }
}
uint32_t CpuDrawStore::AcquireGeometry(Nullable<const GpuMesh::DrawData*> geometry) {
    if (!geometry) return UINT32_MAX;
    auto found = _geometryIndices.find(geometry.Get());
    uint32_t index;
    if (found != _geometryIndices.end())
        index = found->second;
    else {
        if (_freeGeometry.empty()) {
            if (_geometryEntries.size() == UINT32_MAX) RADRAY_ABORT("Geometry plan index exhausted");
            index = static_cast<uint32_t>(_geometryEntries.size());
            _geometryEntries.emplace_back();
            _geometryPlans.emplace_back();
        } else {
            index = _freeGeometry.back();
            _freeGeometry.pop_back();
        }
        _geometryEntries[index] = {geometry, 0, 0};
        _geometryIndices.emplace(geometry.Get(), index);
    }
    auto& entry = _geometryEntries[index];
    ++entry.Users;
    if (entry.Epoch != _epoch) {
        InlineVector<CpuVertexBindingRun, 4> runs;
        const auto& buffers = geometry->VertexBuffers;
        for (uint32_t first = 0; first < buffers.size();) {
            uint32_t end = first + 1;
            while (end < buffers.size() && uint64_t{buffers[end - 1].Binding} + 1 == buffers[end].Binding) ++end;
            runs.push_back({first, end - first});
            first = end;
        }
        entry.Epoch = _epoch;
        if (_geometryPlans[index].Runs != runs) {
            _geometryPlans[index].Runs = runs;
            MarkGeometry(index);
        }
    }
    return index;
}
void CpuDrawStore::ReleaseGeometry(uint32_t index) {
    if (index == UINT32_MAX) return;
    auto& entry = _geometryEntries[index];
    if (--entry.Users != 0) return;
    _geometryIndices.erase(entry.Geometry.Get());
    entry = {};
    _geometryPlans[index].Runs.clear();
    _freeGeometry.push_back(index);
    MarkGeometry(index);
}

uint32_t HashPassName(std::string_view name) noexcept {
    return static_cast<uint32_t>(HashData64(name.data(), name.size()));
}

bool IsMirroredAffine(const Eigen::Matrix4f& localToWorld) noexcept {
    const float* m = localToWorld.data();
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(m[i])) return false;
    }
    const float m00 = m[0], m10 = m[1], m20 = m[2];
    const float m01 = m[4], m11 = m[5], m21 = m[6];
    const float m02 = m[8], m12 = m[9], m22 = m[10];
    const float det = m00 * (m11 * m22 - m12 * m21) - m01 * (m10 * m22 - m12 * m20) + m02 * (m10 * m21 - m11 * m20);
    return std::isfinite(det) && det < 0.0f;
}

render::FrontFace OppositeFrontFace(render::FrontFace face) noexcept {
    return face == render::FrontFace::CCW ? render::FrontFace::CW : render::FrontFace::CCW;
}

size_t CpuDrawStore::KeyHash::operator()(const Key& key) const noexcept {
    return (*this)(KeyRef{key.PrimitiveGeneration, key.SectionIndex, key.PassName, key.Policy});
}

size_t CpuDrawStore::KeyHash::operator()(const KeyRef& key) const noexcept {
    HashCode hash;
    hash.Add(key.PrimitiveGeneration);
    hash.Add(key.SectionIndex);
    hash.Add(HashData64(key.PassName.data(), key.PassName.size()));
    hash.Add(key.Policy.Value);
    return hash.ToHashCode();
}

bool CpuDrawStore::KeyEqual::operator()(const Key& lhs, const KeyRef& rhs) const noexcept {
    return lhs.PrimitiveGeneration == rhs.PrimitiveGeneration && lhs.SectionIndex == rhs.SectionIndex && lhs.PassName == rhs.PassName && lhs.Policy == rhs.Policy;
}

size_t CpuDrawStore::BindingKeyHash::operator()(const BindingKey& key) const noexcept {
    HashCode hash;
    hash.Add(key.ProgramGeneration);
    hash.Add(key.Layout);
    hash.Add(key.Policy);
    hash.Add(key.Revision);
    hash.Add(reinterpret_cast<uintptr_t>(key.Program.Get()));
    return hash.ToHashCode();
}

bool CpuDrawStore::SetActivePolicies(uint64_t serial, std::span<const PassPolicy> policies) {
    vector<PassPolicy> next{policies.begin(), policies.end()};
    std::sort(next.begin(), next.end(), [](const auto& a, const auto& b) { return a.Id.Value < b.Id.Value; });
    for (size_t index = 0; index < next.size(); ++index)
        if (!next[index].Id.IsValid() || next[index].Revision == 0 || next[index].PassName.empty() || !next[index].CompileStatic ||
            (index && next[index - 1].Id == next[index].Id)) return false;
    if (_policySerial == serial) return _policies == next;
    if (!_policySerial) _policyMembershipChanged = true;
    for (const auto& policy : next) {
        const auto old = std::find_if(_policies.begin(), _policies.end(), [&](const auto& value) { return value.Id == policy.Id; });
        if (old != _policies.end() && old->Revision == policy.Revision && *old != policy) return false;
    }
    _policySerial = serial;
    if (_policies == next) return true;
    bool membership = _policies.size() != next.size();
    for (const auto& policy : next) {
        const auto old = std::find_if(_policies.begin(), _policies.end(), [&](const auto& value) { return value.Id == policy.Id; });
        if (old == _policies.end() || old->PassName != policy.PassName) membership = true;
        if (old != _policies.end() && *old != policy && std::find(_dirtyPolicies.begin(), _dirtyPolicies.end(), policy.Id.Value) == _dirtyPolicies.end())
            _dirtyPolicies.push_back(policy.Id.Value);
    }
    _policyMembershipChanged |= membership;
    _policies = std::move(next);
    return true;
}

size_t CpuDrawStore::RecordCount(const MaterialRenderData& material) const noexcept {
    if (!_policySerial) return material.Passes.size();
    size_t count = 0;
    for (const auto& pass : material.Passes)
        for (const auto& policy : _policies) count += pass.PassName == policy.PassName;
    return count;
}

void CpuDrawStore::MarkBinding(uint32_t index) {
    if (_bindingChanged.size() <= index) _bindingChanged.resize(size_t{index} + 1);
    if (!_bindingChanged[index]) {
        _bindingChanged[index] = 1;
        _bindingTouched.push_back(index);
    }
}

void CpuDrawStore::ReleaseBinding(uint32_t index) {
    if (index == UINT32_MAX) return;
    if (--_bindingUsers[index] != 0) return;
    _bindings.erase(*_bindingKeys[index]);
    _bindingKeys[index].reset();
    _bindingData[index] = {};
    _freeBindings.push_back(index);
    MarkBinding(index);
}

void CpuDrawStore::ReleaseCached(const Key& key, const Cached& cached) {
    _layouts.Release(cached.Record.Description.LayoutId);
    ReleaseBinding(cached.Record.BindingRecipe);
    ReleaseState(cached.Record.NormalStateId);
    ReleaseState(cached.Record.MirroredStateId);
    ReleaseGeometry(cached.Record.GeometryBindingPlan);
    if (!key.Policy.IsValid()) return;
    const auto policy = _policyUsers.find(key.Policy.Value);
    if (policy == _policyUsers.end()) return;
    const auto user = policy->second.find(key.PrimitiveGeneration);
    if (user != policy->second.end() && --user->second == 0) policy->second.erase(user);
    if (policy->second.empty()) _policyUsers.erase(policy);
}

void CpuDrawStore::PublishBindings(RenderSceneSnapshot& scene) {
    _changedBindingRanges.clear();
    const bool fresh = scene.BindingRecipes.size() != _bindingData.size();
    scene.BindingRecipes.resize(_bindingData.size());
    if (fresh) {
        scene.BindingRecipes = _bindingData;
        if (!_bindingData.empty()) _changedBindingRanges.push_back({0, static_cast<uint32_t>(_bindingData.size())});
    } else {
        for (const uint32_t index : _bindingTouched) {
            scene.BindingRecipes[index] = _bindingData[index];
            _changedBindingRanges.push_back({index, 1});
        }
    }
    for (const uint32_t index : _bindingTouched) _bindingChanged[index] = 0;
    _bindingTouched.clear();
    _changedGeometryRanges.clear();
    const bool freshGeometry = scene.GeometryBindingPlans.size() != _geometryPlans.size();
    scene.GeometryBindingPlans.resize(_geometryPlans.size());
    if (freshGeometry) {
        scene.GeometryBindingPlans = _geometryPlans;
        if (!_geometryPlans.empty()) _changedGeometryRanges.push_back({0, static_cast<uint32_t>(_geometryPlans.size())});
    } else {
        for (const uint32_t index : _geometryTouched) {
            scene.GeometryBindingPlans[index] = _geometryPlans[index];
            _changedGeometryRanges.push_back({index, 1});
        }
    }
    for (const uint32_t index : _geometryTouched) _geometryChanged[index] = 0;
    _geometryTouched.clear();
}

bool CpuDrawStore::SyncPrimitive(const RenderSceneSnapshot& scene, uint32_t primitiveIndex, vector<DrawRecord>& records, size_t firstRecord) {
    constexpr size_t kMaxIndex = std::numeric_limits<uint32_t>::max();
    const auto& primitive = scene.Primitives[primitiveIndex];
    if (primitive.FirstMeshBatch > scene.MeshBatches.size() || primitive.MeshBatchCount > scene.MeshBatches.size() - primitive.FirstMeshBatch) return false;
    auto& keys = _primitiveKeys[primitive.Generation];
    keys.Epoch = _epoch;
    keys.Packed = primitiveIndex;
    ++_stats.DrawRecordPrimitivesVisited;
    const bool mirrored = IsMirroredAffine(primitive.LocalToWorld);
    for (uint32_t offset = 0; offset < primitive.MeshBatchCount; ++offset) {
        const auto batchIndex = primitive.FirstMeshBatch + offset;
        if (batchIndex >= scene.MeshBatches.size()) return false;
        const auto& batch = scene.MeshBatches[batchIndex];
        if (batch.Material >= scene.Materials.size()) return false;
        const auto& material = scene.Materials[batch.Material];
        for (uint32_t passIndex = 0; passIndex < material.Passes.size(); ++passIndex) {
            for (size_t policyIndex = 0; policyIndex < (_policySerial ? _policies.size() : 1); ++policyIndex) {
                const Nullable<const PassPolicy*> policy = _policySerial ? &_policies[policyIndex] : nullptr;
                if (firstRecord >= kMaxIndex) return false;
                const auto& pass = material.Passes[passIndex];
                if (policy && policy->PassName != pass.PassName) continue;
                const KeyRef key{primitive.Generation, batch.SectionIndex, pass.PassName, policy ? policy->Id : PassPolicyId{}};
                auto found = _cache.find(key);
                const bool inserted = found == _cache.end();
                if (inserted) {
                    found = _cache.try_emplace(Key{key.PrimitiveGeneration, key.SectionIndex, string{key.PassName}, key.Policy}).first;
                    keys.Keys.push_back(found->first);
                    if (policy) ++_policyUsers[policy->Id.Value][primitive.Generation];
                }
                auto& cached = found->second;
                const auto* geometry = batch.Geometry.Get();
                auto* program = pass.Program.Get();
                DrawRecordStatus status = DrawRecordStatus::Ready;
                if (!pass.Valid || !program)
                    status = DrawRecordStatus::InvalidBindings;
                else if (!geometry)
                    status = DrawRecordStatus::InvalidGeometry;
                const bool same = !inserted && cached.Epoch != 0 &&
                                  cached.MaterialGeneration == material.Generation && cached.MaterialStructureRevision == material.StructureRevision &&
                                  primitive.RenderDataRevision != 0 && cached.GeometryRevision == primitive.RenderDataRevision &&
                                  cached.ProgramGeneration == pass.ProgramGeneration &&
                                  cached.PolicyRevision == (policy ? policy->Revision : 0) &&
                                  cached.Geometry.Get() == geometry && cached.Program.Get() == program && cached.PipelineState == pass.PipelineState &&
                                  cached.FirstIndex == batch.FirstIndex && cached.IndexCount == batch.IndexCount &&
                                  cached.VertexOffset == batch.VertexOffset && cached.Queue == material.Queue &&
                                  cached.Status == status;
                if (!same) {
                    cached.MaterialGeneration = material.Generation;
                    cached.MaterialStructureRevision = material.StructureRevision;
                    cached.GeometryRevision = primitive.RenderDataRevision;
                    cached.ProgramGeneration = pass.ProgramGeneration;
                    cached.PolicyRevision = policy ? policy->Revision : 0;
                    cached.Geometry = geometry;
                    cached.Program = program;
                    cached.PipelineState = pass.PipelineState;
                    cached.FirstIndex = batch.FirstIndex;
                    cached.IndexCount = batch.IndexCount;
                    cached.VertexOffset = batch.VertexOffset;
                    cached.Queue = material.Queue;
                    cached.Status = status;
                    if (cached.Record.Id == 0) {
                        if (_nextId == 0) return false;
                        cached.Record.Id = _nextId++;
                        cached.Record.Generation = 1;
                    }
                    ++cached.Record.RecipeRevision;
                    cached.Record.Description.Program = program;
                    cached.Record.Description.PipelineState = pass.PipelineState;
                    cached.Record.Description.Geometry = geometry;
                    const auto oldLayout = cached.Record.Description.LayoutId;
                    cached.Record.Description.LayoutId = geometry ? _layouts.Acquire(geometry->VertexLayout) : PrimitiveVertexLayoutId{};
                    _layouts.Release(oldLayout);
                    cached.Record.Description.FirstIndex = batch.FirstIndex;
                    cached.Record.Description.IndexCount = batch.IndexCount;
                    cached.Record.Description.VertexOffset = batch.VertexOffset;
                    cached.Record.Policy = key.Policy;
                    cached.Record.PolicyRevision = cached.PolicyRevision;
                    cached.Record.Status = status;
                    const uint32_t oldBinding = cached.Record.BindingRecipe;
                    cached.Record.BindingRecipe = UINT32_MAX;
                    if (policy && status == DrawRecordStatus::Ready) {
                        const BindingKey bindingKey{pass.ProgramGeneration, cached.Record.Description.LayoutId.Value, policy->Id.Value, policy->Revision, program};
                        auto binding = _bindings.find(bindingKey);
                        const bool newBinding = binding == _bindings.end();
                        uint32_t bindingIndex;
                        if (newBinding) {
                            if (_freeBindings.empty()) {
                                if (_bindingData.size() == UINT32_MAX) return false;
                                bindingIndex = static_cast<uint32_t>(_bindingData.size());
                                _bindingData.emplace_back();
                                _bindingUsers.push_back(0);
                                _bindingKeys.emplace_back(bindingKey);
                            } else {
                                bindingIndex = _freeBindings.back();
                                _freeBindings.pop_back();
                                _bindingKeys[bindingIndex] = bindingKey;
                            }
                            _bindings.emplace(bindingKey, bindingIndex);
                        } else
                            bindingIndex = binding->second;
                        StaticPassCompileResult compiled;
                        const bool valid = policy->CompileStatic({pass, *geometry, material.Queue, newBinding ? nullptr : &_bindingData[bindingIndex]}, compiled);
                        ++_stats.StaticRecipeCompiles;
                        if (newBinding) {
                            _bindingData[bindingIndex] = compiled.Bindings;
                            ++_stats.BindingRecipeCompiles;
                            MarkBinding(bindingIndex);
                        }
                        ++_bindingUsers[bindingIndex];
                        cached.Record.BindingRecipe = bindingIndex;
                        cached.Record.Description.PipelineState = compiled.NormalState;
                        cached.Record.MirroredState = compiled.MirroredState;
                        if (!valid) cached.Record.Status = DrawRecordStatus::InvalidBindings;
                    } else {
                        cached.Record.MirroredState = pass.PipelineState;
                        cached.Record.MirroredState.Primitive.FaceClockwise = OppositeFrontFace(pass.PipelineState.Primitive.FaceClockwise);
                    }
                    const auto oldNormal = cached.Record.NormalStateId, oldMirrored = cached.Record.MirroredStateId;
                    const auto oldGeometry = cached.Record.GeometryBindingPlan;
                    cached.Record.NormalStateId = AcquireState(cached.Record.Description.PipelineState);
                    cached.Record.MirroredStateId = AcquireState(cached.Record.MirroredState);
                    cached.Record.GeometryBindingPlan = AcquireGeometry(geometry);
                    ReleaseState(oldNormal);
                    ReleaseState(oldMirrored);
                    ReleaseGeometry(oldGeometry);
                    ReleaseBinding(oldBinding);
                    ++_stats.DrawRecordBuilds;
                } else {
                    ++_stats.DrawRecordsReused;
                }
                cached.Epoch = _epoch;
                DrawRecord record = cached.Record;
                record.PrimitiveId = primitive.Id;
                record.Primitive = static_cast<RenderPrimitiveIndex>(primitiveIndex);
                record.Batch = batchIndex;
                record.Material = batch.Material;
                record.SectionIndex = batch.SectionIndex;
                record.PassIndex = passIndex;
                record.PassNameHash = HashPassName(pass.PassName);
                record.ProgramFrameId = pass.ProgramFrameId;
                record.Queue = material.Queue;
                record.LayerMask = primitive.LayerMask;
                if (record.Mirrored != mirrored) ++_stats.DrawRecordStateSelects;
                record.Mirrored = mirrored;
                cached.Record.Mirrored = mirrored;
                if (firstRecord < records.size())
                    records[firstRecord] = record;
                else
                    records.push_back(record);
                ++firstRecord;
                ++_stats.DrawRecordCopies;
            }
        }
    }
    std::erase_if(keys.Keys, [&](const Key& key) {
        const auto found = _cache.find(key);
        if (found == _cache.end()) return true;
        if (found->second.Epoch == _epoch) return false;
        ReleaseCached(found->first, found->second);
        _cache.erase(found);
        return true;
    });
    return true;
}

bool CpuDrawStore::Sync(RenderSceneSnapshot& scene, RenderValidationMode) {
    RADRAY_PROFILE_SCOPE_N("Scene.CompileStaticDraws");
    _stats = {};
    _changedDrawRanges.clear();
    ++_stats.DrawRecordFullSyncs;
    scene.DrawRecords.clear();
    scene.PrimitiveDrawBegin.resize(scene.Primitives.size() + 1);
    if (++_epoch == 0) {
        for (auto& [key, cached] : _cache) ReleaseCached(key, cached);
        _cache.clear();
        _primitiveKeys.clear();
        ++_epoch;
    }
    if (scene.Primitives.size() > std::numeric_limits<uint32_t>::max()) return false;
    scene.DrawRecords.reserve(scene.MeshBatches.size() * 2);
    for (uint32_t index = 0; index < scene.Primitives.size(); ++index) {
        scene.PrimitiveDrawBegin[index] = static_cast<uint32_t>(scene.DrawRecords.size());
        if (!SyncPrimitive(scene, index, scene.DrawRecords, scene.DrawRecords.size())) return false;
    }
    scene.PrimitiveDrawBegin.back() = static_cast<uint32_t>(scene.DrawRecords.size());
    std::erase_if(_cache, [&](const auto& entry) {
        if (entry.second.Epoch == _epoch) return false;
        ReleaseCached(entry.first, entry.second);
        return true;
    });
    std::erase_if(_primitiveKeys, [&](const auto& entry) { return entry.second.Epoch != _epoch; });
    if (_cache.empty()) _layouts.Clear();
    if (!scene.DrawRecords.empty()) _changedDrawRanges.push_back({0, static_cast<uint32_t>(scene.DrawRecords.size())});
    PublishBindings(scene);
    _policyMembershipChanged = false;
    _dirtyPolicies.clear();
    WriteStats(scene);
    return true;
}

bool CpuDrawStore::SyncChanged(RenderSceneSnapshot& scene, std::span<const uint32_t> changedPrimitives,
                               bool layoutChanged, RenderValidationMode validation) {
    if (layoutChanged || _policyMembershipChanged || scene.PrimitiveDrawBegin.size() != scene.Primitives.size() + 1 || _epoch == 0 || _epoch == UINT64_MAX)
        return Sync(scene, validation);
    for (const auto index : _syncPrimitives) _syncMarked[index] = 0;
    _syncPrimitives.clear();
    if (_syncMarked.size() < scene.Primitives.size()) _syncMarked.resize(scene.Primitives.size());
    const auto addPrimitive = [&](uint32_t index) {
        if (index >= scene.Primitives.size()) return false;
        if (!_syncMarked[index]) {
            _syncMarked[index] = 1;
            _syncPrimitives.push_back(index);
        }
        return true;
    };
    for (const auto index : changedPrimitives)
        if (!addPrimitive(index)) return false;
    for (const auto policy : _dirtyPolicies) {
        const auto users = _policyUsers.find(policy);
        if (users == _policyUsers.end()) continue;
        for (const auto& [generation, count] : users->second) {
            const auto primitive = _primitiveKeys.find(generation);
            if (primitive != _primitiveKeys.end() && !addPrimitive(primitive->second.Packed)) return false;
        }
    }
    for (const auto index : _syncPrimitives) {
        if (index >= scene.Primitives.size()) return false;
        const auto& primitive = scene.Primitives[index];
        if (primitive.FirstMeshBatch > scene.MeshBatches.size() || primitive.MeshBatchCount > scene.MeshBatches.size() - primitive.FirstMeshBatch) return false;
        const auto begin = scene.PrimitiveDrawBegin[index], end = scene.PrimitiveDrawBegin[index + 1];
        if (begin > end || end > scene.DrawRecords.size()) return false;
        size_t count = 0;
        for (uint32_t section = 0; section < primitive.MeshBatchCount; ++section) {
            const auto& batch = scene.MeshBatches[primitive.FirstMeshBatch + section];
            if (batch.Material >= scene.Materials.size()) return false;
            count += RecordCount(scene.Materials[batch.Material]);
        }
        if (count != end - begin) return Sync(scene, validation);
    }
    RADRAY_PROFILE_SCOPE_N("Scene.CompileStaticDraws");
    _stats = {};
    _changedDrawRanges.clear();
    ++_epoch;
    for (const auto index : _syncPrimitives) {
        if (!SyncPrimitive(scene, index, scene.DrawRecords, scene.PrimitiveDrawBegin[index])) return false;
        const auto first = scene.PrimitiveDrawBegin[index];
        if (scene.PrimitiveDrawBegin[index + 1] != first) _changedDrawRanges.push_back({first, scene.PrimitiveDrawBegin[index + 1] - first});
    }
    PublishBindings(scene);
    _dirtyPolicies.clear();
    WriteStats(scene);
    return true;
}

void CpuDrawStore::WriteStats(RenderSceneSnapshot& scene) noexcept {
    scene.HasPassPolicies = _policySerial.has_value();
    _stats.DrawRecordBytes = scene.DrawRecords.capacity() * sizeof(DrawRecord);
    scene.Stats.DrawRecordBuilds = _stats.DrawRecordBuilds;
    scene.Stats.DrawRecordsReused = _stats.DrawRecordsReused;
    scene.Stats.DrawRecordStateSelects = _stats.DrawRecordStateSelects;
    scene.Stats.DrawRecordBytes = _stats.DrawRecordBytes;
    scene.Stats.CpuSceneBytes = scene.Primitives.capacity() * sizeof(RenderPrimitiveData) +
                                scene.MeshBatches.capacity() * sizeof(MeshBatch) +
                                scene.Materials.capacity() * sizeof(MaterialRenderData) +
                                scene.Lights.capacity() * sizeof(RenderLightData) +
                                scene.Stats.DrawRecordBytes;
    scene.Stats.DrawRecordFullSyncs = _stats.DrawRecordFullSyncs;
    scene.Stats.DrawRecordPrimitivesVisited = _stats.DrawRecordPrimitivesVisited;
    scene.Stats.DrawRecordCopies = _stats.DrawRecordCopies;
    scene.Stats.StaticRecipeCompiles = _stats.StaticRecipeCompiles;
    scene.Stats.BindingRecipeCompiles = _stats.BindingRecipeCompiles;
    scene.Stats.CpuSceneBytes += scene.BindingRecipes.capacity() * sizeof(StaticBindingRecipe);
    scene.Stats.CpuSceneBytes += scene.GeometryBindingPlans.capacity() * sizeof(CpuGeometryBindingPlan);
}

}  // namespace radray
