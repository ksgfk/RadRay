#include <radray/render/bind_bridge.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

#include <radray/errors.h>
#include <radray/basic_math.h>

namespace radray::render {

BindBridgeLayout::BindBridgeLayout(const HlslShaderDesc& desc, std::span<const BindBridgeStaticSampler> staticSamplers) noexcept
	: _cbStorageBuilder{} {
	this->BuildFromHlsl(desc);
	this->ApplyStaticSamplers(staticSamplers);
}

BindBridgeLayout::BindBridgeLayout(const SpirvShaderDesc& desc, std::span<const BindBridgeStaticSampler> staticSamplers) noexcept
	: _cbStorageBuilder{} {
	this->BuildFromSpirv(desc);
	this->ApplyStaticSamplers(staticSamplers);
}

std::optional<uint32_t> BindBridgeLayout::GetBindingId(std::string_view name) const noexcept {
	auto it = _nameToBindingId.find(string{name});
	if (it == _nameToBindingId.end()) {
		return std::nullopt;
	}
	return it->second;
}

bool BindBridgeLayout::BuildFromHlsl(const HlslShaderDesc& desc) noexcept {
	constexpr uint32_t maxRootDWORD = 64;
	constexpr uint32_t maxRootBYTE = maxRootDWORD * 4;

	enum class HlslRSPlacement {
		Table,
		RootDescriptor,
		RootConstant,
	};

	_bindings.clear();
	_nameToBindingId.clear();

	if (desc.BoundResources.empty()) {
		return true;
	}
	vector<HlslRSPlacement> placements{desc.BoundResources.size(), HlslRSPlacement::Table};
	auto cmpResource = [&](size_t lhs, size_t rhs) noexcept {
		const auto& l = desc.BoundResources[lhs];
		const auto& r = desc.BoundResources[rhs];
		return l.BindPoint < r.BindPoint;
	};
	auto buildTable = [&]() {
		vector<size_t> asTable;
		for (size_t i = 0; i < desc.BoundResources.size(); i++) {
			if (placements[i] != HlslRSPlacement::Table) {
				continue;
			}
			asTable.push_back(i);
		}
		vector<size_t> resourceIndices, samplerIndices;
		for (size_t i : asTable) {
			const auto& binding = desc.BoundResources[i];
			ResourceBindType type = binding.MapResourceBindType();
			if (type == ResourceBindType::Sampler) {
				samplerIndices.push_back(i);
			} else {
				resourceIndices.push_back(i);
			}
		}
		unordered_map<uint32_t, vector<size_t>> resourceSpace, samplerSpace;
		for (size_t i : resourceIndices) {
			resourceSpace[desc.BoundResources[i].Space].push_back(i);
		}
		for (size_t i : samplerIndices) {
			samplerSpace[desc.BoundResources[i].Space].push_back(i);
		}
		vector<vector<size_t>> descriptors;
		auto buildDescriptors = [&](const decltype(resourceSpace)& splits) noexcept {
			for (auto [space, indices] : splits) {
				auto& elements = descriptors.emplace_back();
				elements.reserve(indices.size());
				std::sort(indices.begin(), indices.end(), cmpResource);
				for (size_t i : indices) {
					elements.emplace_back(i);
				}
			}
		};
		buildDescriptors(resourceSpace);
		buildDescriptors(samplerSpace);
		return descriptors;
	};

	bool hasRootConstant = false;
	uint32_t rootConstantSize = 0;
	size_t rootConstantIndex = std::numeric_limits<size_t>::max();
	size_t bestRootConstIndex = std::numeric_limits<size_t>::max();
	for (size_t i = 0; i < desc.BoundResources.size(); i++) {
		const auto& binding = desc.BoundResources[i];
		if (binding.Type == HlslShaderInputType::CBUFFER && binding.BindCount == 1) {
			auto cbufferDataOpt = desc.FindCBufferByName(binding.Name);
			if (!cbufferDataOpt.has_value()) {
				RADRAY_ERR_LOG("{} {}", "BindBridgeLayout", "cannot find cbuffer data");
				return false;
			}
			const auto& cbufferData = cbufferDataOpt.value().get();
			if (cbufferData.Size > maxRootBYTE) {
				continue;
			}
			uint32_t bestBindPoint = std::numeric_limits<uint32_t>::max();
			uint32_t bestSpace = std::numeric_limits<uint32_t>::max();
			if (bestRootConstIndex != std::numeric_limits<size_t>::max()) {
				const auto& bestBinding = desc.BoundResources[bestRootConstIndex];
				bestBindPoint = bestBinding.BindPoint;
				bestSpace = bestBinding.Space;
			}
			if (binding.Space < bestSpace || (binding.Space == bestSpace && binding.BindPoint < bestBindPoint)) {
				bestRootConstIndex = i;
			}
		}
	}
	if (bestRootConstIndex != std::numeric_limits<size_t>::max()) {
		const auto& binding = desc.BoundResources[bestRootConstIndex];
		auto cbufferDataOpt = desc.FindCBufferByName(binding.Name);
		RADRAY_ASSERT(cbufferDataOpt.has_value());
		const auto& cbufferData = cbufferDataOpt.value().get();
		hasRootConstant = true;
		rootConstantSize = cbufferData.Size;
		rootConstantIndex = bestRootConstIndex;
		placements[bestRootConstIndex] = HlslRSPlacement::RootConstant;
	}
	vector<size_t> asRootDesc;
	for (size_t i = 0; i < desc.BoundResources.size(); i++) {
		const auto& binding = desc.BoundResources[i];
		if (placements[i] != HlslRSPlacement::Table) {
			continue;
		}
		if (binding.BindCount != 1) {
			continue;
		}
		ResourceBindType type = binding.MapResourceBindType();
		if (type == ResourceBindType::CBuffer ||
			type == ResourceBindType::Buffer ||
			type == ResourceBindType::RWBuffer) {
			asRootDesc.push_back(i);
			placements[i] = HlslRSPlacement::RootDescriptor;
		}
	}
	vector<vector<size_t>> tables;
	while (true) {
		std::sort(asRootDesc.begin(), asRootDesc.end(), cmpResource);
		tables = buildTable();
		size_t totalDWORD = 0;
		if (hasRootConstant) {
			totalDWORD += Align(rootConstantSize, 4) / 4;
		}
		totalDWORD += asRootDesc.size() * 2;
		totalDWORD += tables.size();
		if (totalDWORD <= maxRootDWORD) {
			break;
		}
		if (hasRootConstant) {
			hasRootConstant = false;
			if (rootConstantIndex != std::numeric_limits<size_t>::max()) {
				asRootDesc.push_back(rootConstantIndex);
				placements[rootConstantIndex] = HlslRSPlacement::RootDescriptor;
			}
			continue;
		}
		if (!asRootDesc.empty()) {
			size_t rmIndex = asRootDesc.back();
			placements[rmIndex] = HlslRSPlacement::Table;
			asRootDesc.pop_back();
			continue;
		}
		RADRAY_ERR_LOG("{} {}", "BindBridgeLayout", "cannot fit into root signature limits");
		return false;
	}
	if (hasRootConstant && rootConstantIndex != std::numeric_limits<size_t>::max()) {
		const auto& binding = desc.BoundResources[rootConstantIndex];
		_bindings.emplace_back(BindingEntry{
			0,
			binding.Name,
			BindingKind::PushConst,
			ResourceBindType::CBuffer,
			1,
			binding.BindPoint,
			binding.Space,
			binding.Stages,
			0,
			0,
			0,
			rootConstantSize,
			{}});
	}

	uint32_t rootIndex = 0;
	for (size_t i : asRootDesc) {
		const auto& binding = desc.BoundResources[i];
		_bindings.emplace_back(BindingEntry{
			0,
			binding.Name,
			BindingKind::RootDescriptor,
			binding.MapResourceBindType(),
			binding.BindCount,
			binding.BindPoint,
			binding.Space,
			binding.Stages,
			0,
			0,
			rootIndex++,
			0,
			{}});
	}

	uint32_t setIndex = 0;
	for (const auto& table : tables) {
		uint32_t elemIndex = 0;
		for (size_t i : table) {
			const auto& binding = desc.BoundResources[i];
			_bindings.emplace_back(BindingEntry{
				0,
				binding.Name,
				BindingKind::DescriptorSet,
				binding.MapResourceBindType(),
				binding.BindCount,
				binding.BindPoint,
				binding.Space,
				binding.Stages,
				setIndex,
				elemIndex++,
				0,
				0,
				{}});
		}
		setIndex++;
	}

	auto builderOpt = CreateCBufferStorageBuilder(desc);
	if (builderOpt.has_value()) {
		_cbStorageBuilder = std::move(builderOpt.value());
	}
	this->BuildBindingIndex();
	return true;
}

bool BindBridgeLayout::BuildFromSpirv(const SpirvShaderDesc& desc) noexcept {
	_bindings.clear();
	_nameToBindingId.clear();

	if (desc.ResourceBindings.empty() && desc.PushConstants.empty()) {
		return true;
	}

	if (!desc.PushConstants.empty()) {
		const auto& pc = desc.PushConstants.front();
		_bindings.emplace_back(BindingEntry{
			0,
			pc.Name,
			BindingKind::PushConst,
			ResourceBindType::CBuffer,
			1,
			0,
			0,
			pc.Stages,
			0,
			0,
			0,
			pc.Size,
			{}});
		if (desc.PushConstants.size() > 1) {
			RADRAY_ERR_LOG("{} {} {}", "BindBridgeLayout", "multiple push constants detected, only the first is used", desc.PushConstants.size());
		}
	}

	unordered_map<uint32_t, vector<const SpirvResourceBinding*>> perSet;
	for (const auto& binding : desc.ResourceBindings) {
		if (binding.Kind == SpirvResourceKind::UNKNOWN) {
			continue;
		}
		perSet[binding.Set].push_back(&binding);
	}

	vector<uint32_t> setIndices;
	setIndices.reserve(perSet.size());
	for (const auto& [setIndex, _] : perSet) {
		setIndices.push_back(setIndex);
	}
	std::sort(setIndices.begin(), setIndices.end());

	uint32_t setOrderIndex = 0;
	for (auto setIndex : setIndices) {
		auto& bindings = perSet[setIndex];
		std::sort(bindings.begin(), bindings.end(), [](const SpirvResourceBinding* a, const SpirvResourceBinding* b) {
			return a->Binding < b->Binding;
		});
		uint32_t elemIndex = 0;
		for (const auto* b : bindings) {
			auto type = b->MapResourceBindType();
			if (type == ResourceBindType::UNKNOWN) {
				continue;
			}
			uint32_t count = b->ArraySize == 0 ? 1u : b->ArraySize;
			_bindings.emplace_back(BindingEntry{
				0,
				b->Name,
				BindingKind::DescriptorSet,
				type,
				count,
				b->Binding,
				b->Set,
				b->Stages,
				setOrderIndex,
				elemIndex++,
				0,
				0,
				{}});
		}
		setOrderIndex++;
	}

	auto builderOpt = CreateCBufferStorageBuilder(desc);
	if (builderOpt.has_value()) {
		_cbStorageBuilder = std::move(builderOpt.value());
	}
	this->BuildBindingIndex();
	return true;
}

void BindBridgeLayout::BuildBindingIndex() noexcept {
	_nameToBindingId.clear();
	uint32_t nextId = 0;
	for (auto& entry : _bindings) {
		entry.Id = nextId++;
		if (!entry.Name.empty()) {
			if (_nameToBindingId.find(entry.Name) == _nameToBindingId.end()) {
				_nameToBindingId.emplace(entry.Name, entry.Id);
			}
		}
	}
}

RootSignatureDescriptorContainer BindBridgeLayout::GetDescriptor() const noexcept {
	RootSignatureDescriptorContainer container{};
	container._staticSamplers.clear();

	vector<std::pair<uint32_t, RootSignatureRootDescriptor>> rootEntries;
	for (const auto& b : _bindings) {
		if (b.Kind != BindingKind::RootDescriptor) {
			continue;
		}
		rootEntries.emplace_back(b.RootIndex, RootSignatureRootDescriptor{
			b.BindPoint,
			b.Space,
			b.Type,
			b.Stages});
	}
	std::sort(rootEntries.begin(), rootEntries.end(), [](const auto& a, const auto& b) {
		return a.first < b.first;
	});
	container._rootDescriptors.reserve(rootEntries.size());
	for (const auto& [_, rd] : rootEntries) {
		container._rootDescriptors.push_back(rd);
	}

	unordered_map<uint32_t, vector<const BindingEntry*>> sets;
	vector<uint32_t> setOrder;
	for (const auto& b : _bindings) {
		if (b.Kind != BindingKind::DescriptorSet) {
			continue;
		}
		if (sets.find(b.SetIndex) == sets.end()) {
			setOrder.push_back(b.SetIndex);
		}
		sets[b.SetIndex].push_back(&b);
	}
	std::sort(setOrder.begin(), setOrder.end());
	container._descriptorSets.reserve(setOrder.size());
	for (auto setIndex : setOrder) {
		auto& elems = sets[setIndex];
		std::sort(elems.begin(), elems.end(), [](const BindingEntry* a, const BindingEntry* b) {
			return a->ElementIndex < b->ElementIndex;
		});
		size_t elemStart = container._elements.size();
		for (const auto* e : elems) {
			RootSignatureSetElement elem{};
			elem.Slot = e->BindPoint;
			elem.Space = e->Space;
			elem.Type = e->Type;
			elem.Count = e->BindCount;
			elem.Stages = e->Stages;
			if (!e->StaticSamplers.empty()) {
				const size_t start = container._staticSamplers.size();
				container._staticSamplers.insert(container._staticSamplers.end(), e->StaticSamplers.begin(), e->StaticSamplers.end());
				elem.StaticSamplers = std::span<const SamplerDescriptor>{
					container._staticSamplers.data() + start,
					e->StaticSamplers.size()};
			} else {
				elem.StaticSamplers = {};
			}
			container._elements.push_back(elem);
		}
		size_t elemCount = container._elements.size() - elemStart;
		RootSignatureDescriptorSet setDesc{};
		setDesc.Elements = std::span<const RootSignatureSetElement>{
			container._elements.data() + elemStart,
			elemCount};
		container._descriptorSets.push_back(setDesc);
	}

	container._desc.RootDescriptors = container._rootDescriptors;
	container._desc.DescriptorSets = container._descriptorSets;
	bool hasPushConst = false;
	for (const auto& b : _bindings) {
		if (b.Kind != BindingKind::PushConst) {
			continue;
		}
		container._desc.Constant = RootSignatureConstant{
			b.BindPoint,
			b.Space,
			b.PushConstSize,
			b.Stages};
		hasPushConst = true;
		break;
	}
	if (!hasPushConst) {
		container._desc.Constant = std::nullopt;
	}
	return container;
}


void BindBridgeLayout::ApplyStaticSamplers(std::span<const BindBridgeStaticSampler> staticSamplers) noexcept {
	for (auto& b : _bindings) {
		if (b.Kind == BindingKind::DescriptorSet && b.Type == ResourceBindType::Sampler) {
			b.StaticSamplers.clear();
		}
	}
	if (staticSamplers.empty()) {
		return;
	}
	for (const auto& ss : staticSamplers) {
		if (ss.Name.empty() || ss.Samplers.empty()) {
			continue;
		}
		bool matched = false;
		for (auto& b : _bindings) {
			if (b.Kind != BindingKind::DescriptorSet || b.Type != ResourceBindType::Sampler || b.Name != ss.Name) {
				continue;
			}
			matched = true;
			if (ss.Samplers.size() != b.BindCount) {
				RADRAY_ERR_LOG("{} {} {} {}", "BindBridgeLayout", "static sampler count mismatch", b.Name, b.BindCount);
				continue;
			}
			b.StaticSamplers = ss.Samplers;
		}
		if (!matched) {
			RADRAY_ERR_LOG("{} {} {}", "BindBridgeLayout", "static sampler name not found", ss.Name);
		}
	}
}

BindBridge::BindBridge(Device* device, RootSignature* rootSig, const BindBridgeLayout& layout) {
	auto storageOpt = layout._cbStorageBuilder.Build();
	if (!storageOpt.has_value()) {
		throw std::runtime_error("BindBridge: failed to build cbuffer storage");
	}
	_cbStorage = std::move(storageOpt.value());

	_bindings.reserve(layout._bindings.size());
	_nameToBindingId = layout._nameToBindingId;
	for (const auto& b : layout._bindings) {
		if (b.Kind == BindBridgeLayout::BindingKind::PushConst) {
			PushConstBinding loc{};
			loc.Size = b.PushConstSize;
			auto var = _cbStorage.GetVar(b.Name);
			loc.CBufferId = var.IsValid() ? var.GetId() : StructuredBufferStorage::InvalidId;
			_bindings.emplace_back(loc);
			continue;
		}
		if (b.Kind == BindBridgeLayout::BindingKind::RootDescriptor) {
			RootDescriptorBinding loc{};
			loc.RootIndex = b.RootIndex;
			loc.Type = b.Type;
			loc.BindCount = b.BindCount;
			if (b.Type == ResourceBindType::CBuffer) {
				auto var = _cbStorage.GetVar(b.Name);
				loc.CBufferId = var.IsValid() ? var.GetId() : StructuredBufferStorage::InvalidId;
			}
			_bindings.emplace_back(loc);
			continue;
		}
		DescriptorSetBindingInfo loc{};
		loc.SetIndex = b.SetIndex;
		loc.ElementIndex = b.ElementIndex;
		loc.Type = b.Type;
		loc.BindCount = b.BindCount;
		if (b.Type == ResourceBindType::CBuffer) {
			auto var = _cbStorage.GetVar(b.Name);
			loc.CBufferId = var.IsValid() ? var.GetId() : StructuredBufferStorage::InvalidId;
		}
		_bindings.emplace_back(loc);
	}

	uint32_t maxRootIndex = 0;
	bool hasRoot = false;
	for (const auto& b : layout._bindings) {
		if (b.Kind != BindBridgeLayout::BindingKind::RootDescriptor) {
			continue;
		}
		hasRoot = true;
		maxRootIndex = std::max(maxRootIndex, b.RootIndex);
	}
	_rootDescViews.assign(hasRoot ? (maxRootIndex + 1) : 0, nullptr);

	unordered_map<uint32_t, vector<const BindBridgeLayout::BindingEntry*>> setBindings;
	uint32_t maxSetIndex = 0;
	bool hasSet = false;
	for (const auto& b : layout._bindings) {
		if (b.Kind != BindBridgeLayout::BindingKind::DescriptorSet) {
			continue;
		}
		hasSet = true;
		maxSetIndex = std::max(maxSetIndex, b.SetIndex);
		setBindings[b.SetIndex].push_back(&b);
	}
	_descSets.assign(hasSet ? (maxSetIndex + 1) : 0, DescSetRecord{});
	for (auto& [setIndex, bindings] : setBindings) {
		auto& record = _descSets[setIndex];
		record.Set = nullptr;
		std::sort(bindings.begin(), bindings.end(), [](const auto* a, const auto* b) {
			return a->ElementIndex < b->ElementIndex;
		});
		record.Bindings.reserve(bindings.size());
		for (const auto* e : bindings) {
			DescSetBinding binding{};
			binding.Slot = e->BindPoint;
			binding.Count = e->BindCount;
			binding.Type = e->Type;
			binding.Views.assign(e->BindCount, nullptr);
			if (e->Type == ResourceBindType::CBuffer) {
				auto var = _cbStorage.GetVar(e->Name);
				binding.CBufferId = var.IsValid() ? var.GetId() : StructuredBufferStorage::InvalidId;
			}
			record.Bindings.emplace_back(std::move(binding));
		}
	}

	_ownedDescriptorSets.clear();
	_ownedDescriptorSets.reserve(_descSets.size());
	for (uint32_t i = 0; i < _descSets.size(); ++i) {
		auto setOpt = device->CreateDescriptorSet(rootSig, i);
		if (!setOpt.HasValue()) {
			throw std::runtime_error("BindBridge: CreateDescriptorSet failed");
		}
		auto set = setOpt.Release();
		this->SetDescriptorSet(i, set.get());
		_ownedDescriptorSets.emplace_back(std::move(set));
	}
}

std::optional<uint32_t> BindBridge::GetBindingId(std::string_view name) const noexcept {
	auto it = _nameToBindingId.find(string{name});
	if (it == _nameToBindingId.end()) {
		return std::nullopt;
	}
	return it->second;
}

bool BindBridge::SetResource(uint32_t id, ResourceView* view, uint32_t arrayIndex) noexcept {
	if (id >= _bindings.size()) {
		RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::ArgumentOutOfRange, "id");
		return false;
	}
	const auto& binding = _bindings[id];
	if (std::holds_alternative<PushConstBinding>(binding)) {
		RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::InvalidArgument, "push constant");
		return false;
	}
	if (const auto* root = std::get_if<RootDescriptorBinding>(&binding)) {
		if (root->Type == ResourceBindType::Sampler) {
			RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::InvalidArgument, "sampler");
			return false;
		}
		if (arrayIndex >= root->BindCount) {
			RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::ArgumentOutOfRange, "arrayIndex");
			return false;
		}
		this->SetRootDescriptor(root->RootIndex, view);
		return true;
	}
	const auto* desc = std::get_if<DescriptorSetBindingInfo>(&binding);
	if (!desc) {
		return false;
	}
	if (desc->Type == ResourceBindType::Sampler) {
		RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::InvalidArgument, "sampler");
		return false;
	}
	if (arrayIndex >= desc->BindCount) {
		RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::ArgumentOutOfRange, "arrayIndex");
		return false;
	}
	this->SetDescriptorSetResource(desc->SetIndex, desc->ElementIndex, arrayIndex, view);
	return true;
}

bool BindBridge::SetResource(std::string_view name, ResourceView* view, uint32_t arrayIndex) noexcept {
	auto idOpt = this->GetBindingId(name);
	if (!idOpt.has_value()) {
		RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::InvalidArgument, "name");
		return false;
	}
	return this->SetResource(idOpt.value(), view, arrayIndex);
}

StructuredBufferView BindBridge::GetCBuffer(uint32_t id) noexcept {
	if (id >= _bindings.size()) {
		return {};
	}
	const auto& binding = _bindings[id];
	StructuredBufferId cbId = StructuredBufferStorage::InvalidId;
	std::visit([&](const auto& b) {
		cbId = b.CBufferId;
	}, binding);
	if (cbId == StructuredBufferStorage::InvalidId) {
		return {};
	}
	return StructuredBufferView{&_cbStorage, cbId};
}

StructuredBufferReadOnlyView BindBridge::GetCBuffer(uint32_t id) const noexcept {
	if (id >= _bindings.size()) {
		return {};
	}
	const auto& binding = _bindings[id];
	StructuredBufferId cbId = StructuredBufferStorage::InvalidId;
	std::visit([&](const auto& b) {
		cbId = b.CBufferId;
	}, binding);
	if (cbId == StructuredBufferStorage::InvalidId) {
		return {};
	}
	return StructuredBufferReadOnlyView{&_cbStorage, cbId};
}

void BindBridge::SetRootDescriptor(uint32_t slot, ResourceView* view) noexcept {
	if (slot >= _rootDescViews.size()) {
		RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::ArgumentOutOfRange, "slot");
		return;
	}
	_rootDescViews[slot] = view;
}

void BindBridge::SetDescriptorSet(uint32_t setIndex, DescriptorSet* set) noexcept {
	if (setIndex >= _descSets.size()) {
		RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::ArgumentOutOfRange, "setIndex");
		return;
	}
	_descSets[setIndex].Set = set;
}

void BindBridge::SetDescriptorSetResource(uint32_t setIndex, uint32_t elementIndex, uint32_t arrayIndex, ResourceView* view) noexcept {
	if (setIndex >= _descSets.size()) {
		RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::ArgumentOutOfRange, "setIndex");
		return;
	}
	auto& record = _descSets[setIndex];
	if (elementIndex >= record.Bindings.size()) {
		RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::ArgumentOutOfRange, "elementIndex");
		return;
	}
	auto& binding = record.Bindings[elementIndex];
	if (arrayIndex >= binding.Views.size()) {
		RADRAY_ERR_LOG("{} {} '{}'", Errors::InvalidOperation, Errors::ArgumentOutOfRange, "arrayIndex");
		return;
	}
	binding.Views[arrayIndex] = view;
}

void BindBridge::Bind(CommandEncoder* encoder) const noexcept {
	if (!encoder) {
		return;
	}
	for (const auto& binding : _bindings) {
		const auto* pc = std::get_if<PushConstBinding>(&binding);
		if (!pc || pc->CBufferId == StructuredBufferStorage::InvalidId || pc->Size == 0) {
			continue;
		}
		auto span = _cbStorage.GetSpan(pc->CBufferId);
		size_t size = std::min(span.size(), static_cast<size_t>(pc->Size));
		if (size > 0) {
			encoder->PushConstant(span.data(), size);
		}
		break;
	}

	for (size_t i = 0; i < _rootDescViews.size(); i++) {
		if (_rootDescViews[i]) {
			encoder->BindRootDescriptor(static_cast<uint32_t>(i), _rootDescViews[i]);
		}
	}

	for (size_t si = 0; si < _descSets.size(); si++) {
		const auto& record = _descSets[si];
		if (!record.Set) {
			continue;
		}
		for (size_t ei = 0; ei < record.Bindings.size(); ei++) {
			const auto& binding = record.Bindings[ei];
			if (binding.Type == ResourceBindType::Sampler) {
				continue;
			}
			for (size_t ai = 0; ai < binding.Views.size(); ai++) {
				auto* view = binding.Views[ai];
				if (view) {
					record.Set->SetResource(static_cast<uint32_t>(ei), static_cast<uint32_t>(ai), view);
				}
			}
		}
		encoder->BindDescriptorSet(static_cast<uint32_t>(si), record.Set);
	}
}


bool BindBridge::Upload(Device& device, CBufferArena& arena) noexcept {
	auto& storage = _cbStorage;

	_ownedCBufferViews.clear();
	uint32_t alignment = device.GetDetail().CBufferAlignment;
	if (alignment == 0) {
		alignment = 1;
	}

	for (uint32_t id = 0; id < _bindings.size(); ++id) {
		const auto& binding = _bindings[id];
		const auto* root = std::get_if<RootDescriptorBinding>(&binding);
		const auto* desc = std::get_if<DescriptorSetBindingInfo>(&binding);
		if (!root && !desc) {
			continue;
		}
		ResourceBindType type = root ? root->Type : desc->Type;
		if (type != ResourceBindType::CBuffer) {
			continue;
		}
		uint32_t bindCount = root ? root->BindCount : desc->BindCount;
		if (bindCount == 0) {
			bindCount = 1;
		}
		auto rootView = this->GetCBuffer(id);
		if (!rootView) {
			continue;
		}
		for (uint32_t arrayIndex = 0; arrayIndex < bindCount; ++arrayIndex) {
			StructuredBufferView view = rootView;
			if (rootView.GetSelf().GetArraySize() > 0) {
				view = rootView.GetArrayElement(arrayIndex);
			} else if (arrayIndex > 0) {
				continue;
			}
			if (!view) {
				continue;
			}
			auto span = storage.GetSpan(view.GetId(), view.GetArrayIndex());
			size_t uploadSize = Align(span.size(), alignment);
			auto alloc = arena.Allocate(uploadSize);
			if (!alloc.Target || !alloc.Mapped) {
				RADRAY_ERR_LOG("{} {}", "BindBridge", "CBufferArena allocation failed");
				return false;
			}
			if (!span.empty()) {
				std::memcpy(alloc.Mapped, span.data(), span.size());
			}
			if (uploadSize > span.size()) {
				std::memset(static_cast<byte*>(alloc.Mapped) + span.size(), 0, uploadSize - span.size());
			}
			BufferViewDescriptor viewDesc{};
			viewDesc.Target = alloc.Target;
			viewDesc.Range = BufferRange{alloc.Offset, uploadSize};
			viewDesc.Stride = 0;
			viewDesc.Format = TextureFormat::UNKNOWN;
			viewDesc.Usage = BufferUse::CBuffer;
			auto bvOpt = device.CreateBufferView(viewDesc);
			if (!bvOpt.HasValue()) {
				RADRAY_ERR_LOG("{} {}", "BindBridge", "CreateBufferView failed");
				return false;
			}
			auto bv = bvOpt.Release();
			_ownedCBufferViews.emplace_back(std::move(bv));
			this->SetResource(id, _ownedCBufferViews.back().get(), arrayIndex);
		}
	}
	return true;
}

}  // namespace radray::render
