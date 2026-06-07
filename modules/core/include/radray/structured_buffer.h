#pragma once

#include <optional>
#include <span>

#include <radray/types.h>
#include <radray/basic_math.h>
#include <radray/logger.h>

#include <type_traits>

namespace radray {

struct StructuredBufferId {
    constexpr static size_t InvalidValue = std::numeric_limits<size_t>::max();
    constexpr static StructuredBufferId Invalid() { return StructuredBufferId{InvalidValue}; }

    size_t Value{InvalidValue};

    constexpr StructuredBufferId() noexcept = default;
    constexpr StructuredBufferId(size_t value) noexcept : Value(value) {}

    constexpr operator size_t() const noexcept { return Value; }

    friend auto operator<=>(const StructuredBufferId&, const StructuredBufferId&) = default;
};

class StructuredBufferVariable;
class StructuredBufferType;
class StructuredBufferStorage;
template <typename TStorage>
class BasicStructuredBufferView;
using StructuredBufferView = BasicStructuredBufferView<StructuredBufferStorage>;
using StructuredBufferReadOnlyView = BasicStructuredBufferView<const StructuredBufferStorage>;

class StructuredBufferVariable {
public:
    StructuredBufferVariable() = default;
    StructuredBufferVariable(string name, StructuredBufferId typeId) : _name(std::move(name)), _typeId(typeId) {}

    std::string_view GetName() const noexcept { return _name; }
    StructuredBufferId GetTypeId() const noexcept { return _typeId; }
    size_t GetOffset() const noexcept { return _offset; }
    size_t GetArraySize() const noexcept { return _arraySize; }
    StructuredBufferId GetGlobalId() const noexcept { return _globalId; }

private:
    string _name;
    StructuredBufferId _typeId{StructuredBufferId::Invalid()};
    size_t _offset{0};
    size_t _arraySize{0};
    StructuredBufferId _globalId{StructuredBufferId::Invalid()};

    friend class StructuredBufferStorage;
};

class StructuredBufferType {
public:
    StructuredBufferType() = default;
    StructuredBufferType(std::string_view name, StructuredBufferId id) : _name(string{name}), _id(id) {}

    std::string_view GetName() const noexcept { return _name; }
    StructuredBufferId GetId() const noexcept { return _id; }
    size_t GetSizeInBytes() const noexcept { return _size; }
    std::span<const StructuredBufferVariable> GetMembers() const noexcept { return _members; }

private:
    string _name;
    StructuredBufferId _id{StructuredBufferId::Invalid()};
    vector<StructuredBufferVariable> _members;
    size_t _size{0};

    friend class StructuredBufferStorage;
};

class StructuredBufferStorage {
public:
    static constexpr StructuredBufferId InvalidId{StructuredBufferId::Invalid()};

    class Builder {
    public:
        static constexpr StructuredBufferId Invalid = InvalidId;
        struct Member {
            string Name;
            size_t Offset{0};
            StructuredBufferId TypeIndex{Invalid};
            size_t ArraySize{0};
            auto operator<=>(const Member&) const = default;
        };
        struct Type {
            string Name;
            vector<Member> Members;
            size_t SizeInBytes{0};
            auto operator<=>(const Type&) const = default;
        };
        struct Root {
            string Name;
            StructuredBufferId TypeIndex{Invalid};
            size_t ArraySize{0};
        };

        StructuredBufferId AddType(std::string_view name, size_t size) noexcept;
        void AddMemberForType(StructuredBufferId targetType, StructuredBufferId memberType, std::string_view name, size_t offset) noexcept;
        void AddMemberForType(StructuredBufferId targetType, StructuredBufferId memberType, std::string_view name, size_t offset, size_t arraySize) noexcept;
        StructuredBufferId AddRoot(std::string_view name, StructuredBufferId typeIndex) noexcept;
        StructuredBufferId AddRoot(std::string_view name, StructuredBufferId typeIndex, size_t arraySize) noexcept;
        void SetAlignment(size_t align) noexcept;

        bool IsValid() const noexcept;
        std::optional<StructuredBufferStorage> Build() const noexcept;

    private:
        void BuildMember(StructuredBufferStorage& storage) const noexcept;

        vector<Type> _types;
        vector<Root> _roots;
        size_t _align{0};
    };

    StructuredBufferView GetVar(std::string_view name) noexcept;
    StructuredBufferReadOnlyView GetVar(std::string_view name) const noexcept;
    void WriteData(size_t offset, std::span<const byte> data) noexcept;
    std::span<const byte> GetData() const noexcept { return _buffer; }
    std::span<const byte> GetGlobalSpan(size_t offset, size_t size) const noexcept;
    std::span<const byte> GetSpan(StructuredBufferId globalId) const noexcept;
    std::span<const byte> GetSpan(StructuredBufferId globalId, size_t arrayIndex) const noexcept;

private:
    struct GlobalVarIndexer {
        StructuredBufferId ParentTypeId;
        size_t MemberIndexInType;
        size_t GlobalOffset;
    };

    vector<StructuredBufferType> _types;
    vector<StructuredBufferVariable> _rootVarIds;
    vector<GlobalVarIndexer> _globalIndex;
    vector<byte> _buffer;

    template <typename T>
    friend class BasicStructuredBufferView;
};

template <typename TStorage>
class BasicStructuredBufferView {
public:
    using ViewType = BasicStructuredBufferView<TStorage>;
    using ConstView = BasicStructuredBufferView<const std::remove_const_t<TStorage>>;

    BasicStructuredBufferView() = default;
    BasicStructuredBufferView(TStorage* storage, StructuredBufferId globalId, size_t arrayIndex = 0) noexcept
        : _storage(storage), _globalId(globalId), _arrayIndex(arrayIndex) {}

    operator ConstView() const noexcept
    requires(!std::is_const_v<TStorage>)
    {
        return ConstView(_storage, _globalId, _arrayIndex);
    }

    bool IsValid() const noexcept { return _storage != nullptr; }
    operator bool() const noexcept { return IsValid(); }

    ViewType GetVar(std::string_view name) const noexcept {
        RADRAY_ASSERT(this->IsValid());
        const auto& type = this->GetType();
        for (const auto& mem : type.GetMembers()) {
            if (mem.GetName() == name) {
                return ViewType{_storage, mem.GetGlobalId()};
            }
        }
        return ViewType{};
    }

    ViewType GetArrayElement(size_t index) const noexcept {
        RADRAY_ASSERT(this->IsValid());
        const auto& var = this->GetSelf();
        if (var.GetArraySize() == 0 || index >= var.GetArraySize()) {
            return ViewType{};
        }
        return ViewType{_storage, var.GetGlobalId(), index};
    }

    StructuredBufferId GetId() const noexcept { return _globalId; }
    size_t GetArrayIndex() const noexcept { return _arrayIndex; }

    size_t GetGlobalOffset() const noexcept {
        RADRAY_ASSERT(this->IsValid());
        const auto& indexer = _storage->_globalIndex[_globalId];
        if (_arrayIndex == 0) {
            return indexer.GlobalOffset;
        }
        const auto& var = this->GetSelf();
        if (var.GetArraySize() == 0) {
            return indexer.GlobalOffset;
        }
        const auto& type = _storage->_types[var.GetTypeId()];
        return indexer.GlobalOffset + _arrayIndex * type.GetSizeInBytes();
    }

    const StructuredBufferType& GetType() const noexcept {
        RADRAY_ASSERT(this->IsValid());
        const auto& indexer = _storage->_globalIndex[_globalId];
        const auto& var = (indexer.ParentTypeId == StructuredBufferId::Invalid())
                              ? _storage->_rootVarIds[indexer.MemberIndexInType]
                              : _storage->_types[indexer.ParentTypeId].GetMembers()[indexer.MemberIndexInType];
        return _storage->_types[var.GetTypeId()];
    }

    const StructuredBufferVariable& GetSelf() const noexcept {
        RADRAY_ASSERT(this->IsValid());
        const auto& indexer = _storage->_globalIndex[_globalId];
        const auto& var = (indexer.ParentTypeId == StructuredBufferId::Invalid())
                              ? _storage->_rootVarIds[indexer.MemberIndexInType]
                              : _storage->_types[indexer.ParentTypeId].GetMembers()[indexer.MemberIndexInType];
        return var;
    }

    ConstView AsReadOnly() const noexcept {
        return ConstView{_storage, _globalId, _arrayIndex};
    }

    template <class T>
    void SetValue(const T& value) noexcept
    requires(!std::is_const_v<TStorage>)
    {
        if (!this->IsValid()) {
            return;
        }
        if constexpr (std::is_trivially_copyable_v<T>) {
            _storage->WriteData(this->GetGlobalOffset(), std::as_bytes(std::span{&value, 1}));
        } else if constexpr (radray::IsEigenMatrix<T>::value || radray::IsEigenVector<T>::value) {
            using Scalar = typename T::Scalar;
            constexpr size_t Count = size_t(T::RowsAtCompileTime) * size_t(T::ColsAtCompileTime);
            _storage->WriteData(this->GetGlobalOffset(), std::as_bytes(std::span<const Scalar, Count>{value.data(), Count}));
        } else {
            static_assert(std::is_trivially_copyable_v<T>, "BasicStructuredBufferView::SetValue requires trivially copyable type or fixed-size Eigen matrix/vector");
        }
    }

private:
    TStorage* _storage{nullptr};
    StructuredBufferId _globalId{StructuredBufferId::Invalid()};
    size_t _arrayIndex{0};
};

extern template class BasicStructuredBufferView<StructuredBufferStorage>;
extern template class BasicStructuredBufferView<const StructuredBufferStorage>;

}  // namespace radray
