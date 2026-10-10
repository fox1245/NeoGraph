#pragma once

#include <sqlite3.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace neograph::test::tool_effects {

// A process-local, scoped VFS forwards every operation to SQLite's real VFS.
// Only the selected journal's WAL xSync can fail. This exercises COMMIT I/O,
// unlike INSERT/UPDATE triggers, without changing production broker interfaces.
// Declare this before the host so wrapped SQLite files close before destruction.
class ScopedWalSyncFault {
public:
    ScopedWalSyncFault() : parent_(sqlite3_vfs_find(nullptr)) {
        if (!parent_) throw std::runtime_error("SQLite default VFS is unavailable");
        static std::atomic<std::uint64_t> next{0};
        name_ = "ng-tool-effect-sync-fault-" + std::to_string(++next);
        vfs_ = *parent_;
        vfs_.pNext = nullptr;
        vfs_.zName = name_.c_str();
        vfs_.pAppData = this;
        vfs_.szOsFile = static_cast<int>(offset() + parent_->szOsFile);
        vfs_.xOpen = open;
        vfs_.xDelete = remove;
        vfs_.xAccess = access;
        vfs_.xFullPathname = full_path;
        vfs_.xDlOpen = dl_open;
        vfs_.xDlError = dl_error;
        vfs_.xDlSym = dl_sym;
        vfs_.xDlClose = dl_close;
        vfs_.xRandomness = randomness;
        vfs_.xSleep = sleep;
        vfs_.xCurrentTime = current_time;
        vfs_.xGetLastError = last_error;
        if (vfs_.iVersion >= 2) vfs_.xCurrentTimeInt64 = current_time_int64;
        if (vfs_.iVersion >= 3) {
            vfs_.xSetSystemCall = set_system_call;
            vfs_.xGetSystemCall = get_system_call;
            vfs_.xNextSystemCall = next_system_call;
        }
        if (sqlite3_vfs_register(&vfs_, 1) != SQLITE_OK)
            throw std::runtime_error("cannot install scoped SQLite sync-fault VFS");
    }
    ~ScopedWalSyncFault() {
        sqlite3_vfs_unregister(&vfs_);
        sqlite3_vfs_register(parent_, 1);
    }
    ScopedWalSyncFault(const ScopedWalSyncFault&) = delete;
    ScopedWalSyncFault& operator=(const ScopedWalSyncFault&) = delete;

    void target(const std::string& database) { wal_ = database + "-wal"; }
    void enable() { fail_.store(true); }
    void disable() { fail_.store(false); }
    std::uint64_t failures() const { return failures_.load(); }

private:
    struct File {
        sqlite3_file outer;
        sqlite3_file* inner;
        ScopedWalSyncFault* fault;
    };
    static constexpr std::size_t offset() {
        constexpr auto alignment = alignof(std::max_align_t);
        return (sizeof(File) + alignment - 1) / alignment * alignment;
    }
    static ScopedWalSyncFault& self(sqlite3_vfs* vfs) {
        return *static_cast<ScopedWalSyncFault*>(vfs->pAppData);
    }
    static File& file(sqlite3_file* raw) { return *reinterpret_cast<File*>(raw); }
    static int open(sqlite3_vfs* vfs, const char* name, sqlite3_file* raw,
                    int flags, int* out_flags) {
        auto& fault = self(vfs);
        if (!name || fault.wal_ != name)
            return fault.parent_->xOpen(fault.parent_, name, raw, flags, out_flags);
        auto& f = file(raw);
        f.outer.pMethods = nullptr;
        f.inner = reinterpret_cast<sqlite3_file*>(reinterpret_cast<unsigned char*>(raw) + offset());
        f.inner->pMethods = nullptr;
        f.fault = &fault;
        const int result = fault.parent_->xOpen(fault.parent_, name, f.inner, flags, out_flags);
        if (f.inner->pMethods) f.outer.pMethods = &methods_;
        return result;
    }
    static int close(sqlite3_file* raw) {
        auto& f = file(raw);
        const int result = f.inner->pMethods->xClose(f.inner);
        f.outer.pMethods = nullptr;
        return result;
    }
    static int read(sqlite3_file* raw, void* buffer, int size, sqlite3_int64 at) {
        auto* f = file(raw).inner;
        return f->pMethods->xRead(f, buffer, size, at);
    }
    static int write(sqlite3_file* raw, const void* buffer, int size, sqlite3_int64 at) {
        auto* f = file(raw).inner;
        return f->pMethods->xWrite(f, buffer, size, at);
    }
    static int truncate(sqlite3_file* raw, sqlite3_int64 size) {
        auto* f = file(raw).inner;
        return f->pMethods->xTruncate(f, size);
    }
    static int sync(sqlite3_file* raw, int flags) {
        auto& f = file(raw);
        if (f.fault->fail_.load()) {
            f.fault->failures_.fetch_add(1);
            return SQLITE_IOERR_FSYNC;
        }
        return f.inner->pMethods->xSync(f.inner, flags);
    }
    static int size(sqlite3_file* raw, sqlite3_int64* out) {
        auto* f = file(raw).inner;
        return f->pMethods->xFileSize(f, out);
    }
    static int lock(sqlite3_file* raw, int mode) {
        auto* f = file(raw).inner;
        return f->pMethods->xLock(f, mode);
    }
    static int unlock(sqlite3_file* raw, int mode) {
        auto* f = file(raw).inner;
        return f->pMethods->xUnlock(f, mode);
    }
    static int reserved(sqlite3_file* raw, int* out) {
        auto* f = file(raw).inner;
        return f->pMethods->xCheckReservedLock(f, out);
    }
    static int control(sqlite3_file* raw, int op, void* arg) {
        auto* f = file(raw).inner;
        return f->pMethods->xFileControl(f, op, arg);
    }
    static int sector(sqlite3_file* raw) {
        auto* f = file(raw).inner;
        return f->pMethods->xSectorSize(f);
    }
    static int characteristics(sqlite3_file* raw) {
        auto* f = file(raw).inner;
        return f->pMethods->xDeviceCharacteristics(f);
    }
    // WAL handles use v1 I/O; shared memory and mmap belong to the unwrapped
    // database handle. SQLite never requests them on this wrapped WAL handle.
    inline static const sqlite3_io_methods methods_{
        1, close, read, write, truncate, sync, size, lock, unlock, reserved,
        control, sector, characteristics, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

    static int remove(sqlite3_vfs* vfs, const char* path, int sync_dir) {
        auto* p = self(vfs).parent_;
        return p->xDelete(p, path, sync_dir);
    }
    static int access(sqlite3_vfs* vfs, const char* path, int flags, int* out) {
        auto* p = self(vfs).parent_;
        return p->xAccess(p, path, flags, out);
    }
    static int full_path(sqlite3_vfs* vfs, const char* path, int size, char* out) {
        auto* p = self(vfs).parent_;
        return p->xFullPathname(p, path, size, out);
    }
    static void* dl_open(sqlite3_vfs* vfs, const char* path) {
        auto* p = self(vfs).parent_;
        return p->xDlOpen(p, path);
    }
    static void dl_error(sqlite3_vfs* vfs, int size, char* out) {
        auto* p = self(vfs).parent_;
        p->xDlError(p, size, out);
    }
    static void (*dl_sym(sqlite3_vfs* vfs, void* handle, const char* symbol))(void) {
        auto* p = self(vfs).parent_;
        return p->xDlSym(p, handle, symbol);
    }
    static void dl_close(sqlite3_vfs* vfs, void* handle) {
        auto* p = self(vfs).parent_;
        p->xDlClose(p, handle);
    }
    static int randomness(sqlite3_vfs* vfs, int size, char* out) {
        auto* p = self(vfs).parent_;
        return p->xRandomness(p, size, out);
    }
    static int sleep(sqlite3_vfs* vfs, int micros) {
        auto* p = self(vfs).parent_;
        return p->xSleep(p, micros);
    }
    static int current_time(sqlite3_vfs* vfs, double* out) {
        auto* p = self(vfs).parent_;
        return p->xCurrentTime(p, out);
    }
    static int last_error(sqlite3_vfs* vfs, int size, char* out) {
        auto* p = self(vfs).parent_;
        return p->xGetLastError ? p->xGetLastError(p, size, out) : 0;
    }
    static int current_time_int64(sqlite3_vfs* vfs, sqlite3_int64* out) {
        auto* p = self(vfs).parent_;
        return p->xCurrentTimeInt64(p, out);
    }
    static int set_system_call(sqlite3_vfs* vfs, const char* name, sqlite3_syscall_ptr call) {
        auto* p = self(vfs).parent_;
        return p->xSetSystemCall ? p->xSetSystemCall(p, name, call) : SQLITE_NOTFOUND;
    }
    static sqlite3_syscall_ptr get_system_call(sqlite3_vfs* vfs, const char* name) {
        auto* p = self(vfs).parent_;
        return p->xGetSystemCall ? p->xGetSystemCall(p, name) : nullptr;
    }
    static const char* next_system_call(sqlite3_vfs* vfs, const char* name) {
        auto* p = self(vfs).parent_;
        return p->xNextSystemCall ? p->xNextSystemCall(p, name) : nullptr;
    }

    sqlite3_vfs* parent_;
    sqlite3_vfs vfs_{};
    std::string name_, wal_;
    std::atomic<bool> fail_{false};
    std::atomic<std::uint64_t> failures_{0};
};

}  // namespace neograph::test::tool_effects
