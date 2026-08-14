#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string_view>

#include <radray/types.h>

// radraycore 对 LMDB (Lightning Memory-Mapped Database, https://github.com/LMDB/lmdb)
// 的薄封装。LMDB 是 OLDAP-2.8 许可的纯 C 单文件键排序存储 (mdb.c + midl.c), 本封装把
// MDB_env / MDB_txn / MDB_cursor 收进 RAII, 只对外暴露 byte[] -> byte[] 的存取。
// 与 radray/json.h (yyjson) / radray/xml.h (pugixml) 同模式: 公开头只前置声明 LMDB 的
// 不透明类型, 不把 <lmdb.h> 暴露给上层。
struct MDB_env;
struct MDB_txn;
struct MDB_cursor;

namespace radray {

/// LMDB 操作的抽象结果: 把 MDB_* 返回码收敛成三个语义, 不让 LMDB 的错误码泄漏到上层。
enum class LmdbResult : uint8_t {
    Ok,       ///< 成功。
    NotFound, ///< 未命中 (目标 key 不存在 / 游标到末尾), 不是错误。
    Failure,  ///< 其他错误 (IO、map 满、参数错误等), 详情见 outError。
};

/// LMDB 命名数据库 (DBI) 句柄。对应 MDB_dbi (unsigned int)。
using LmdbDatabase = uint32_t;

/// 键 / 值的字节视图。LMDB 的 key 与 value 都是任意字节串, 本视图不拥有内存。
struct LmdbValue {
    const void* Data{nullptr};
    size_t Size{0};

    LmdbValue() noexcept = default;
    LmdbValue(const void* data, size_t size) noexcept : Data(data), Size(size) {}
    LmdbValue(std::string_view data) noexcept : Data(data.data()), Size(data.size()) {}
    LmdbValue(std::span<const byte> data) noexcept : Data(data.data()), Size(data.size()) {}
};

/// LMDB 环境 (一个库)。对应 MDB_env, 落盘为单文件 (MDB_NOSUBDIR)。
///
/// 【单写者】同时最多一个写事务; 读事务互不阻塞。本封装不做跨线程同步, 调用方保证
/// 单线程使用 (与 AssetDatabase 的单线程契约一致)。同一线程内, 读事务须在写事务 begin
/// 之前 commit/abort —— LMDB 不允许一个读事务跨越另一个写事务的提交。
class LmdbEnvironment {
public:
    LmdbEnvironment() noexcept = default;
    ~LmdbEnvironment();
    LmdbEnvironment(const LmdbEnvironment&) = delete;
    LmdbEnvironment& operator=(const LmdbEnvironment&) = delete;
    LmdbEnvironment(LmdbEnvironment&& other) noexcept;
    LmdbEnvironment& operator=(LmdbEnvironment&& other) noexcept;

    /// 打开 (必要时创建) path 处的库。path 是单文件 (MDB_NOSUBDIR)。
    /// mapSize 是地址映射上限, 应在预期数据量之上留余量; readOnly=true 时不创建文件。
    /// 失败返回 false 并填 outError。
    bool Open(const std::filesystem::path& path, uint64_t mapSize, bool readOnly = false, string* outError = nullptr);

    /// 关闭并释放环境。未打开时是空操作。
    void Close() noexcept;

    bool IsOpen() const noexcept { return _env != nullptr; }

    /// 底层 MDB_env 句柄。仅供本封装内部与测试使用。
    MDB_env* Handle() const noexcept { return _env; }

private:
    MDB_env* _env{nullptr};
};

/// LMDB 事务。所有读写都经事务; 读事务与写事务由 readOnly 区分。
class LmdbTransaction {
public:
    LmdbTransaction() noexcept = default;
    ~LmdbTransaction();
    LmdbTransaction(const LmdbTransaction&) = delete;
    LmdbTransaction& operator=(const LmdbTransaction&) = delete;
    LmdbTransaction(LmdbTransaction&& other) noexcept;
    LmdbTransaction& operator=(LmdbTransaction&& other) noexcept;

    bool Begin(LmdbEnvironment& env, bool readOnly, string* outError = nullptr);
    bool Commit(string* outError = nullptr);
    void Abort() noexcept;

    bool IsActive() const noexcept { return _txn != nullptr; }
    bool IsReadOnly() const noexcept { return _readOnly; }

    /// 打开 (必要时创建) 命名数据库。create=true 需要写事务。outDb 只在成功时写入。
    bool OpenDatabase(std::string_view name, bool create, LmdbDatabase& outDb, string* outError = nullptr);

    /// 读 key 的值。命中 → Ok 并填 outValue; 未命中 → NotFound; 错误 → Failure。
    LmdbResult Get(LmdbDatabase db, LmdbValue key, vector<byte>& outValue, string* outError = nullptr) const;

    /// 覆盖写入。成功返回 true。
    bool Put(LmdbDatabase db, LmdbValue key, LmdbValue value, string* outError = nullptr);

    /// 仅在 key 不存在时写入。key 已存在 → NotFound; 错误 → Failure。
    LmdbResult PutNoOverwrite(LmdbDatabase db, LmdbValue key, LmdbValue value, string* outError = nullptr);

    /// 删除 key。命中 → Ok; 未命中 → NotFound; 错误 → Failure。
    LmdbResult Delete(LmdbDatabase db, LmdbValue key, string* outError = nullptr);

    MDB_txn* Handle() const noexcept { return _txn; }

private:
    MDB_txn* _txn{nullptr};
    bool _readOnly{false};
};

/// LMDB 游标。按 key 字节序遍历一个数据库; 生命周期依附于创建它的事务。
class LmdbCursor {
public:
    LmdbCursor() noexcept = default;
    ~LmdbCursor();
    LmdbCursor(const LmdbCursor&) = delete;
    LmdbCursor& operator=(const LmdbCursor&) = delete;
    LmdbCursor(LmdbCursor&& other) noexcept;
    LmdbCursor& operator=(LmdbCursor&& other) noexcept;

    /// 在事务上打开游标。outCursor 只在成功时写入。
    bool Open(LmdbTransaction& txn, LmdbDatabase db, string* outError = nullptr);

    /// 定位到第一个条目并读出 key / value。空库 → NotFound。
    LmdbResult First(LmdbValue& outKey, vector<byte>& outValue, string* outError = nullptr);
    /// 前进到下一个条目并读出 key / value。到末尾 → NotFound。
    LmdbResult Next(LmdbValue& outKey, vector<byte>& outValue, string* outError = nullptr);

    void Close() noexcept;
    bool IsOpen() const noexcept { return _cursor != nullptr; }

private:
    MDB_cursor* _cursor{nullptr};
};

}  // namespace radray
