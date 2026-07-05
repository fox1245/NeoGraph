// jarvis/src/memory/json_file_store.h
//
// 파일 영속 Store — InMemoryStore 를 감싸고 변이(put/delete)마다 JSON
// 파일로 flush, 시작 시 파일에서 복원한다.
//
// 왜 필요한가: 자비스의 대화 기억(turns/prefs)은 long-term Store 에
// 사는데, InMemoryStore 는 프로세스와 함께 죽는다. 재시작하면 "아까
// 뭐 물어봤지?" 가 통하지 않는 재시작 기억상실이 생김. 쿡북 규모에선
// SQLite 까지 갈 것 없이 JSON 파일 전체 덤프로 충분하다.
//
// 쓰기는 임시파일 + rename 으로 원자적 — 도중에 죽어도 파일이 깨지지 않음.

#pragma once

#include <neograph/graph/store.h>

#include <mutex>
#include <string>

namespace jarvis::memory {

class JsonFileStore : public neograph::graph::Store {
  public:
    /// @param path 영속 파일 경로. 존재하면 시작 시 복원, 없으면 새로 만든다.
    explicit JsonFileStore(std::string path);

    void put(const neograph::graph::Namespace& ns, const std::string& key,
             const neograph::json& value) override;
    std::optional<neograph::graph::StoreItem>
    get(const neograph::graph::Namespace& ns, const std::string& key) const override;
    std::vector<neograph::graph::StoreItem>
    search(const neograph::graph::Namespace& ns_prefix, int limit = 100) const override;
    void delete_item(const neograph::graph::Namespace& ns,
                     const std::string& key) override;
    std::vector<neograph::graph::Namespace>
    list_namespaces(const neograph::graph::Namespace& prefix = {}) const override;

  private:
    void load_from_file();
    void flush_to_file() const;

    std::string                     path_;
    neograph::graph::InMemoryStore  inner_;
    mutable std::mutex              io_mutex_;  // 파일 I/O 직렬화
};

}  // namespace jarvis::memory
