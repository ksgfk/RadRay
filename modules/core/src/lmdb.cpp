#include <radray/lmdb.h>

#include <lmdb.h>

#include <radray/logger.h>

namespace radray {
namespace {

/// 允许的最大命名数据库 (DBI) 数。asset 元数据本轮只用一张 assets 表, 留出余量。
constexpr MDB_dbi kMaxDatabases = 16;

/// 把 MDB_* 返回码映射到抽象结果; 附带错误时填 outError。
LmdbResult ToResult(int rc, string* outError) {
    if (rc == MDB_SUCCESS) {
        return LmdbResult::Ok;
    }
    if (rc == MDB_NOTFOUND) {
        return LmdbResult::NotFound;
    }
    if (outError != nullptr) {
        *outError = mdb_strerror(rc);
    }
    return LmdbResult::Failure;
}

bool Fail(int rc, string* outError) {
    if (rc == MDB_SUCCESS) {
        return true;
    }
    if (outError != nullptr) {
        *outError = mdb_strerror(rc);
    }
    return false;
}

MDB_val ToVal(LmdbValue value) {
    return MDB_val{value.Size, const_cast<void*>(value.Data)};
}

}  // namespace

LmdbEnvironment::~LmdbEnvironment() {
    Close();
}

LmdbEnvironment::LmdbEnvironment(LmdbEnvironment&& other) noexcept : _env(other._env) {
    other._env = nullptr;
}

LmdbEnvironment& LmdbEnvironment::operator=(LmdbEnvironment&& other) noexcept {
    if (this != &other) {
        Close();
        _env = other._env;
        other._env = nullptr;
    }
    return *this;
}

bool LmdbEnvironment::Open(const std::filesystem::path& path, uint64_t mapSize, bool readOnly, string* outError) {
    Close();

    MDB_env* env = nullptr;
    int rc = mdb_env_create(&env);
    if (!Fail(rc, outError)) {
        return false;
    }
    if (!Fail(mdb_env_set_mapsize(env, static_cast<size_t>(mapSize)), outError)) {
        mdb_env_close(env);
        return false;
    }
    // LMDB 默认只允许打开无名主库; 用命名数据库 (assets 等) 必须先放宽 DBI 上限。
    if (!Fail(mdb_env_set_maxdbs(env, kMaxDatabases), outError)) {
        mdb_env_close(env);
        return false;
    }

    const unsigned int flags = MDB_NOSUBDIR | (readOnly ? MDB_RDONLY : 0u);
    const string pathText = path.string();
    rc = mdb_env_open(env, pathText.c_str(), flags, 0664);
    if (!Fail(rc, outError)) {
        mdb_env_close(env);
        return false;
    }

    _env = env;
    return true;
}

void LmdbEnvironment::Close() noexcept {
    if (_env != nullptr) {
        mdb_env_close(_env);
        _env = nullptr;
    }
}

LmdbTransaction::~LmdbTransaction() {
    Abort();
}

LmdbTransaction::LmdbTransaction(LmdbTransaction&& other) noexcept
    : _txn(other._txn), _readOnly(other._readOnly) {
    other._txn = nullptr;
}

LmdbTransaction& LmdbTransaction::operator=(LmdbTransaction&& other) noexcept {
    if (this != &other) {
        Abort();
        _txn = other._txn;
        _readOnly = other._readOnly;
        other._txn = nullptr;
    }
    return *this;
}

bool LmdbTransaction::Begin(LmdbEnvironment& env, bool readOnly, string* outError) {
    Abort();
    if (!env.IsOpen()) {
        if (outError != nullptr) {
            *outError = "LmdbTransaction::Begin: environment is not open";
        }
        return false;
    }

    const unsigned int flags = readOnly ? MDB_RDONLY : 0u;
    MDB_txn* txn = nullptr;
    if (!Fail(mdb_txn_begin(env.Handle(), nullptr, flags, &txn), outError)) {
        return false;
    }
    _txn = txn;
    _readOnly = readOnly;
    return true;
}

bool LmdbTransaction::Commit(string* outError) {
    if (_txn == nullptr) {
        if (outError != nullptr) {
            *outError = "LmdbTransaction::Commit: no active transaction";
        }
        return false;
    }
    MDB_txn* txn = _txn;
    _txn = nullptr;
    const int rc = mdb_txn_commit(txn);
    if (rc != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        if (outError != nullptr) {
            *outError = mdb_strerror(rc);
        }
        return false;
    }
    return true;
}

void LmdbTransaction::Abort() noexcept {
    if (_txn != nullptr) {
        mdb_txn_abort(_txn);
        _txn = nullptr;
    }
}

bool LmdbTransaction::OpenDatabase(std::string_view name, bool create, LmdbDatabase& outDb, string* outError) {
    if (_txn == nullptr) {
        if (outError != nullptr) {
            *outError = "LmdbTransaction::OpenDatabase: no active transaction";
        }
        return false;
    }
    if (create && _readOnly) {
        if (outError != nullptr) {
            *outError = "LmdbTransaction::OpenDatabase: cannot create a database in a read-only transaction";
        }
        return false;
    }

    const string nameText(name);
    MDB_dbi dbi = 0;
    const unsigned int flags = create ? MDB_CREATE : 0u;
    if (!Fail(mdb_dbi_open(_txn, nameText.c_str(), flags, &dbi), outError)) {
        return false;
    }
    outDb = dbi;
    return true;
}

LmdbResult LmdbTransaction::Get(LmdbDatabase db, LmdbValue key, vector<byte>& outValue, string* outError) const {
    if (_txn == nullptr) {
        if (outError != nullptr) {
            *outError = "LmdbTransaction::Get: no active transaction";
        }
        return LmdbResult::Failure;
    }
    MDB_val k = ToVal(key);
    MDB_val v{0, nullptr};
    const int rc = mdb_get(_txn, db, &k, &v);
    if (rc != MDB_SUCCESS) {
        return ToResult(rc, outError);
    }
    const auto* begin = static_cast<const byte*>(v.mv_data);
    outValue.assign(begin, begin + v.mv_size);
    return LmdbResult::Ok;
}

bool LmdbTransaction::Put(LmdbDatabase db, LmdbValue key, LmdbValue value, string* outError) {
    if (_txn == nullptr) {
        if (outError != nullptr) {
            *outError = "LmdbTransaction::Put: no active transaction";
        }
        return false;
    }
    MDB_val k = ToVal(key);
    MDB_val v = ToVal(value);
    return Fail(mdb_put(_txn, db, &k, &v, 0), outError);
}

LmdbResult LmdbTransaction::PutNoOverwrite(LmdbDatabase db, LmdbValue key, LmdbValue value, string* outError) {
    if (_txn == nullptr) {
        if (outError != nullptr) {
            *outError = "LmdbTransaction::PutNoOverwrite: no active transaction";
        }
        return LmdbResult::Failure;
    }
    MDB_val k = ToVal(key);
    MDB_val v = ToVal(value);
    const int rc = mdb_put(_txn, db, &k, &v, MDB_NOOVERWRITE);
    if (rc == MDB_KEYEXIST) {
        return LmdbResult::NotFound;
    }
    return ToResult(rc, outError);
}

LmdbResult LmdbTransaction::Delete(LmdbDatabase db, LmdbValue key, string* outError) {
    if (_txn == nullptr) {
        if (outError != nullptr) {
            *outError = "LmdbTransaction::Delete: no active transaction";
        }
        return LmdbResult::Failure;
    }
    MDB_val k = ToVal(key);
    MDB_val v{0, nullptr};
    const int rc = mdb_del(_txn, db, &k, &v);
    return ToResult(rc, outError);
}

LmdbCursor::~LmdbCursor() {
    Close();
}

LmdbCursor::LmdbCursor(LmdbCursor&& other) noexcept : _cursor(other._cursor) {
    other._cursor = nullptr;
}

LmdbCursor& LmdbCursor::operator=(LmdbCursor&& other) noexcept {
    if (this != &other) {
        Close();
        _cursor = other._cursor;
        other._cursor = nullptr;
    }
    return *this;
}

bool LmdbCursor::Open(LmdbTransaction& txn, LmdbDatabase db, string* outError) {
    Close();
    if (!txn.IsActive()) {
        if (outError != nullptr) {
            *outError = "LmdbCursor::Open: no active transaction";
        }
        return false;
    }
    MDB_cursor* cursor = nullptr;
    if (!Fail(mdb_cursor_open(txn.Handle(), db, &cursor), outError)) {
        return false;
    }
    _cursor = cursor;
    return true;
}

void LmdbCursor::Close() noexcept {
    if (_cursor != nullptr) {
        mdb_cursor_close(_cursor);
        _cursor = nullptr;
    }
}

LmdbResult LmdbCursor::First(LmdbValue& outKey, vector<byte>& outValue, string* outError) {
    if (_cursor == nullptr) {
        if (outError != nullptr) {
            *outError = "LmdbCursor::First: cursor is not open";
        }
        return LmdbResult::Failure;
    }
    MDB_val k{0, nullptr};
    MDB_val v{0, nullptr};
    const int rc = mdb_cursor_get(_cursor, &k, &v, MDB_FIRST);
    if (rc != MDB_SUCCESS) {
        return ToResult(rc, outError);
    }
    outKey = LmdbValue{k.mv_data, k.mv_size};
    const auto* begin = static_cast<const byte*>(v.mv_data);
    outValue.assign(begin, begin + v.mv_size);
    return LmdbResult::Ok;
}

LmdbResult LmdbCursor::Next(LmdbValue& outKey, vector<byte>& outValue, string* outError) {
    if (_cursor == nullptr) {
        if (outError != nullptr) {
            *outError = "LmdbCursor::Next: cursor is not open";
        }
        return LmdbResult::Failure;
    }
    MDB_val k{0, nullptr};
    MDB_val v{0, nullptr};
    const int rc = mdb_cursor_get(_cursor, &k, &v, MDB_NEXT);
    if (rc != MDB_SUCCESS) {
        return ToResult(rc, outError);
    }
    outKey = LmdbValue{k.mv_data, k.mv_size};
    const auto* begin = static_cast<const byte*>(v.mv_data);
    outValue.assign(begin, begin + v.mv_size);
    return LmdbResult::Ok;
}

}  // namespace radray
