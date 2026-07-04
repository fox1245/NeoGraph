// jarvis/src/memory/json_file_store.cpp

#include "json_file_store.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>

namespace jarvis::memory {

using neograph::graph::Namespace;
using neograph::graph::StoreItem;
using neograph::json;

JsonFileStore::JsonFileStore(std::string path) : path_(std::move(path)) {
    load_from_file();
}

// ─────────────────────────────────────────────────────────────────────────────
// Store 인터페이스 — 읽기는 위임, 변이는 위임 + flush
// ─────────────────────────────────────────────────────────────────────────────

void JsonFileStore::put(const Namespace& ns, const std::string& key,
                        const json& value) {
    inner_.put(ns, key, value);
    flush_to_file();
}

std::optional<StoreItem> JsonFileStore::get(const Namespace& ns,
                                            const std::string& key) const {
    return inner_.get(ns, key);
}

std::vector<StoreItem> JsonFileStore::search(const Namespace& ns_prefix,
                                             int limit) const {
    return inner_.search(ns_prefix, limit);
}

void JsonFileStore::delete_item(const Namespace& ns, const std::string& key) {
    inner_.delete_item(ns, key);
    flush_to_file();
}

std::vector<Namespace> JsonFileStore::list_namespaces(
    const Namespace& prefix) const {
    return inner_.list_namespaces(prefix);
}

// ─────────────────────────────────────────────────────────────────────────────
// 파일 I/O
//
// 형식: [{ "ns": ["jarvis","tony"], "key": "turns", "value": ... }, ...]
// 타임스탬프는 복원 시 put() 이 새로 찍는다 — 쿡북 기억 용도에는 무해.
// ─────────────────────────────────────────────────────────────────────────────

void JsonFileStore::load_from_file() {
    std::ifstream f(path_);
    if (!f) return;  // 파일 없음 — 첫 실행

    json doc;
    try {
        std::ostringstream ss;
        ss << f.rdbuf();
        doc = json::parse(ss.str());
    } catch (const std::exception& e) {
        std::cerr << "[JsonFileStore] 기억 파일 파싱 실패(" << path_
                  << "): " << e.what() << " — 빈 기억으로 시작\n";
        return;
    }

    if (!doc.is_array()) return;

    size_t restored = 0;
    for (const auto& item : doc) {
        if (!item.is_object() || !item.contains("ns") || !item.contains("key"))
            continue;
        Namespace ns;
        for (const auto& part : item["ns"]) {
            if (part.is_string()) ns.push_back(part.get<std::string>());
        }
        const std::string key = item["key"].get<std::string>();
        inner_.put(ns, key, item.value("value", json()));
        ++restored;
    }
    std::cerr << "[JsonFileStore] 기억 복원 완료 — " << restored
              << "개 항목 (" << path_ << ")\n";
}

void JsonFileStore::flush_to_file() const {
    std::lock_guard<std::mutex> lock(io_mutex_);

    // 빈 prefix = 전체 열거
    auto all = inner_.search({}, std::numeric_limits<int>::max());

    json doc = json::array();
    for (const auto& item : all) {
        json ns_arr = json::array();
        for (const auto& part : item.ns) ns_arr.push_back(part);
        doc.push_back(json{
            {"ns",    ns_arr},
            {"key",   item.key},
            {"value", item.value}
        });
    }

    // 원자적 쓰기: tmp 에 다 쓴 뒤 rename
    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) {
            std::cerr << "[JsonFileStore] 기억 파일 쓰기 실패: " << tmp << "\n";
            return;
        }
        f << doc.dump();
    }
    if (std::rename(tmp.c_str(), path_.c_str()) != 0) {
        std::cerr << "[JsonFileStore] 기억 파일 rename 실패: " << path_ << "\n";
    }
}

}  // namespace jarvis::memory
