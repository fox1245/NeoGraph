// Optional Linux LD_PRELOAD diagnostic. It never records SQL, parameters, or
// connection strings. Whole-process totals include setup and warmup; elapsed
// time inside these APIs is not a disjoint ProgramRuntime CPU breakdown.
// Build: cc -O2 -shared -fPIC -I/usr/include/postgresql program_store_profile.c
//        -o program_store_profile.so -ldl -pthread
// Run with NEOGRAPH_COST_STORE_PROFILE=/absolute/report.json and LD_PRELOAD.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <libpq-fe.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

enum { BEGIN, COMMIT, SELECT, INSERT, UPDATE, DELETE, OTHER, CATEGORIES };
static const char* names[] = {"begin", "commit", "select", "insert", "update", "delete", "other"};
static _Atomic uint64_t counts[3][CATEGORIES], times[3][CATEGORIES];
static int (*real_step)(sqlite3_stmt*);
static const char* (*real_sql)(sqlite3_stmt*);
static int (*real_sqlite_exec)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
static _Thread_local unsigned sqlite_exec_depth;
static PGresult* (*real_exec)(PGconn*, const char*);
static PGresult* (*real_params)(PGconn*, const char*, int, const Oid*,
                              const char* const*, const int*, const int*, int);
static pthread_once_t initialized = PTHREAD_ONCE_INIT;

static void initialize(void) {
    real_step = dlsym(RTLD_NEXT, "sqlite3_step");
    real_sql = dlsym(RTLD_NEXT, "sqlite3_sql");
    real_sqlite_exec = dlsym(RTLD_NEXT, "sqlite3_exec");
    real_exec = dlsym(RTLD_NEXT, "PQexec");
    real_params = dlsym(RTLD_NEXT, "PQexecParams");
}

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static int category(const char* sql) {
    if (!sql) return OTHER;
    while (*sql == ' ' || *sql == '\n' || *sql == '\t' || *sql == '\r') ++sql;
    for (int i = 0; i < OTHER; ++i) {
        const size_t n = strlen(names[i]);
        if (strncasecmp(sql, names[i], n) == 0 &&
            (sql[n] == '\0' || sql[n] == ' ' || sql[n] == ';' || sql[n] == '\n')) return i;
    }
    return OTHER;
}

static void record(int api, int kind, uint64_t elapsed) {
    atomic_fetch_add_explicit(&counts[api][kind], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&times[api][kind], elapsed, memory_order_relaxed);
}

int sqlite3_step(sqlite3_stmt* statement) {
    pthread_once(&initialized, initialize);
    if (!real_step || !real_sql) abort();
    const int kind = category(real_sql(statement));
    const uint64_t start = now_ns();
    const int result = real_step(statement);
    if (!sqlite_exec_depth) record(0, kind, now_ns() - start);
    return result;
}

int sqlite3_exec(sqlite3* db, const char* sql, int (*callback)(void*, int, char**, char**),
                 void* argument, char** error) {
    pthread_once(&initialized, initialize);
    if (!real_sqlite_exec) abort();
    const int kind = category(sql);
    ++sqlite_exec_depth;
    const uint64_t start = now_ns();
    const int result = real_sqlite_exec(db, sql, callback, argument, error);
    const uint64_t elapsed = now_ns() - start;
    --sqlite_exec_depth;
    if (!sqlite_exec_depth) record(1, kind, elapsed);
    return result;
}

PGresult* PQexec(PGconn* connection, const char* sql) {
    pthread_once(&initialized, initialize);
    if (!real_exec) abort();
    const int kind = category(sql);
    const uint64_t start = now_ns();
    PGresult* result = real_exec(connection, sql);
    record(2, kind, now_ns() - start);
    return result;
}

PGresult* PQexecParams(PGconn* connection, const char* sql, int n, const Oid* types,
                     const char* const* values, const int* lengths, const int* formats, int format) {
    pthread_once(&initialized, initialize);
    if (!real_params) abort();
    const int kind = category(sql);
    const uint64_t start = now_ns();
    PGresult* result = real_params(connection, sql, n, types, values, lengths, formats, format);
    record(2, kind, now_ns() - start);
    return result;
}

__attribute__((destructor)) static void report(void) {
    const char* path = getenv("NEOGRAPH_COST_STORE_PROFILE");
    if (!path || !*path) return;
    FILE* file = fopen(path, "wx");
    if (!file) return;
    fputs("{\"schema_version\":1,\"scope\":\"whole process API calls including setup; sqlite steps include row iteration\",\"apis\":{", file);
    static const char* apis[] = {"sqlite_step", "sqlite_exec", "libpq_sync"};
    for (int api = 0; api < 3; ++api) {
        fprintf(file, "%s\"%s\":{", api ? "," : "", apis[api]);
        for (int kind = 0; kind < CATEGORIES; ++kind) {
            fprintf(file, "%s\"%s\":{\"calls\":%llu,\"total_us\":%.3f}", kind ? "," : "",
                    names[kind], (unsigned long long)atomic_load(&counts[api][kind]),
                    atomic_load(&times[api][kind]) / 1000.0);
        }
        fputc('}', file);
    }
    fputs("}}\n", file);
    fclose(file);
}
