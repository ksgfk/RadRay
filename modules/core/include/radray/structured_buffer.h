#pragma once

#include <optional>
#include <span>

#include <radray/types.h>
#include <radray/nullable.h>
#include <radray/basic_math.h>

namespace radray {

struct StructuredBufferId {
    static constexpr size_t Invalid = std::numeric_limits<size_t>::max();

    size_t Value{Invalid};

    constexpr StructuredBufferId() noexcept = default;
    constexpr StructuredBufferId(size_t value) noexcept : Value(value) {}

    constexpr operator size_t() const noexcept { return Value; }

    friend auto operator<=>(const StructuredBufferId&, const StructuredBufferId&) = default;
};

class StructuredBufferVariable;
class StructuredBufferType;
class StructuredBufferStorage;
class StructuredBufferView;

class StructuredBufferVariable {
public:
    StructuredBufferVariable() = default;
    StructuredBufferVariable(string name, StructuredBufferId typeId) : _name(std::move(name)), _typeId(typeId) {}

    std::string_view GetName() const noexcept { return _name; }
    StructuredBufferId GetTypeId() const noexcept { return _typeId; }
    size_t GetOffset() const noexcept { return _offset; }

private:
    string _name;
    StructuredBufferId _typeId{StructuredBufferId::Invalid};
    size_t _offset{0};

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
    StructuredBufferId _id{StructuredBufferId::Invalid};
    vector<StructuredBufferVariable> _members;
    size_t _size{0};

    friend class StructuredBufferStorage;
};

class StructuredBufferStorage {
public:
    static constexpr StructuredBufferId InvalidId{StructuredBufferId::Invalid};

    class Builder {
    public:
        static constexpr StructuredBufferId Invalid = InvalidId;
        struct Member {
            string Name;
            size_t Offset{0};
            StructuredBufferId TypeIndex{Invalid};
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
        };

        StructuredBufferId AddType(std::string_view name, size_t size) noexcept;
        void AddMemberForType(StructuredBufferId targetType, StructuredBufferId memberType, std::string_view name, size_t offset) noexcept;
        StructuredBufferId AddRoot(std::string_view name, StructuredBufferId typeIndex) noexcept;
        void SetAlignment(size_t align) noexcept;

        bool IsValid() const noexcept;
        std::optional<StructuredBufferStorage> Build() noexcept;

    private:
        void BuildMember(StructuredBufferStorage& storage) noexcept;

        vector<Type> _types;
        vector<Root> _roots;
        size_t _align{0};
    };

    StructuredBufferView GetVar(std::string_view name) noexcept;
    void WriteData(size_t offset, std::span<const byte> data) noexcept;
    std::span<const byte> GetData() const noexcept { return _buffer; }
    std::span<const byte> GetSpan(size_t offset, size_t size) const noexcept;
    std::span<const byte> GetSpan(StructuredBufferId memberId) const noexcept;

private:
    struct GlobalVarIndexer {
        StructuredBufferId TypeId;
        size_t MemberIndexInType;
        size_t GlobalOffset;
    };

    vector<StructuredBufferType> _types;
    vector<StructuredBufferVariable> _rootVarIds;
    vector<GlobalVarIndexer> _globalIndex;
    vector<byte> _buffer;

    friend class StructuredBufferView;
};

class StructuredBufferView {
public:
    StructuredBufferView() = default;
    StructuredBufferView(StructuredBufferStorage* storage, StructuredBufferId memberId) noexcept : _storage(storage), _memberId(memberId) {}

    bool IsValid() const noexcept { return _storage != nullptr && _memberId.Value != StructuredBufferId::Invalid; }
    operator bool() const noexcept { return IsValid(); }

    StructuredBufferView GetVar(std::string_view name) noexcept;

    StructuredBufferId GetId() const noexcept { return _memberId; }
    size_t GetOffset() const noexcept;
    Nullable<StructuredBufferType*> GetType() noexcept;
    Nullable<const StructuredBufferType*> GetType() const noexcept;

    template <class T>
    void SetValue(const T& value) noexcept {
        if (!IsValid()) {
            return;
        }
        if constexpr (std::is_trivially_copyable_v<T>) {
            _storage->WriteData(GetOffset(), std::as_bytes(std::span{&value, 1}));
        } else if constexpr (radray::IsEigenMatrix<T>::value || radray::IsEigenVector<T>::value) {
            using Scalar = typename T::Scalar;
            constexpr size_t Count = size_t(T::RowsAtCompileTime) * size_t(T::ColsAtCompileTime);
            _storage->WriteData(GetOffset(), std::as_bytes(std::span<const Scalar, Count>{value.data(), Count}));
        } else {
            static_assert(std::is_trivially_copyable_v<T>, "StructuredBufferView::SetValue requires trivially copyable type or fixed-size Eigen matrix/vector");
        }
    }

private:
    StructuredBufferStorage* _storage{nullptr};
    StructuredBufferId _memberId{StructuredBufferId::Invalid};
};

}  // namespace radray
