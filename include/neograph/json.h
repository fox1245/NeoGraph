/**
 * @file json.h
 * @brief Thin C++ RAII wrapper around yyjson with nlohmann-compatible API.
 *
 * Replaces nlohmann/json in NeoGraph. Uses yyjson (C) for parsing and
 * serialization while preserving value semantics and a subset of the
 * nlohmann API that NeoGraph actually uses:
 *
 *   - Type queries: is_null / is_bool / is_number / is_string / is_array / is_object
 *   - Factories: json::object(), json::array(), json::parse(), initializer-list ctor
 *   - Access: operator[], at(), value(), get<T>(), contains(), size(), empty()
 *   - Mutation: operator[] assignment, push_back()
 *   - Iteration: range-based for, items() (for structured bindings)
 *   - Serialization: dump()
 *   - ADL helpers: to_json / from_json
 *
 * Value semantics: copy constructor performs a deep copy. operator[] returns
 * a json handle that shares the underlying yyjson document, allowing write-
 * through via rvalue operator= (the `j["key"] = value` pattern).
 */
#pragma once

#include <yyjson.h>

#include <cstdint>
#include <initializer_list>
#include <iosfwd>
#include <ostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace neograph {

class json {
public:
    // ----- Exceptions -----
    class exception : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };
    class parse_error : public exception {
    public:
        using exception::exception;
    };
    class type_error : public exception {
    public:
        using exception::exception;
    };
    class out_of_range : public exception {
    public:
        using exception::exception;
    };

    // ----- Construction -----
    json();                                      // null
    json(std::nullptr_t);
    json(bool b);
    json(int i);
    json(unsigned i);
    json(long i);
    json(unsigned long i);
    json(long long i);
    json(unsigned long long i);
    json(double d);
    json(float f);
    json(const char* s);
    json(const std::string& s);
    json(std::string_view s);
    json(const std::vector<std::string>& v);

    json(std::initializer_list<json> il);

    // ----- Copy / move -----
    json(const json& other);
    json(json&& other) noexcept;
    json& operator=(const json& other) &;
    json& operator=(json&& other) & noexcept;

    // rvalue assignment: write-through to parent if this handle came from operator[]
    json& operator=(const json& other) &&;
    json& operator=(json&& other) && noexcept;

    ~json();

    // ----- Factories -----
    static json object();
    static json array();
    static json array(std::initializer_list<json> il);

    static json parse(std::string_view s);
    static json parse(std::istream& in);

    // ----- Type queries -----
    bool is_null() const noexcept;
    bool is_boolean() const noexcept;
    bool is_bool() const noexcept { return is_boolean(); }
    bool is_number() const noexcept;
    bool is_number_integer() const noexcept;
    bool is_number_unsigned() const noexcept;
    bool is_number_float() const noexcept;
    bool is_string() const noexcept;
    bool is_array() const noexcept;
    bool is_object() const noexcept;
    bool is_primitive() const noexcept;  // not null and not container

    // ----- Size -----
    size_t size() const noexcept;
    bool empty() const noexcept;

    // ----- Access (mutable — get-or-create) -----
    json operator[](const char* key);
    json operator[](const std::string& key);
    json operator[](size_t idx);
    json operator[](int idx) { return (*this)[static_cast<size_t>(idx)]; }

    // ----- Access (const — read-only) -----
    json operator[](const char* key) const;
    json operator[](const std::string& key) const;
    json operator[](size_t idx) const;
    json operator[](int idx) const { return (*this)[static_cast<size_t>(idx)]; }

    // ----- at (throws on missing) -----
    json at(const char* key);
    json at(const std::string& key);
    json at(size_t idx);
    json at(int idx) { return at(static_cast<size_t>(idx)); }
    json at(const char* key) const;
    json at(const std::string& key) const;
    json at(size_t idx) const;
    json at(int idx) const { return at(static_cast<size_t>(idx)); }

    // ----- Membership -----
    bool contains(const char* key) const;
    bool contains(const std::string& key) const;

    // ----- Extraction -----
    template <typename T> T get() const;

    // ----- Safe get with default (object-scoped) -----
    template <typename T> T value(const std::string& key, const T& default_val) const;
    std::string value(const std::string& key, const char* default_val) const;

    // ----- Mutation -----
    void push_back(const json& v);
    void push_back(json&& v);

    // ----- Serialization -----
    std::string dump(int indent = -1) const;

    // ----- Equality -----
    bool operator==(const json& other) const;
    bool operator!=(const json& other) const { return !(*this == other); }

    // ----- Iteration -----
    class iterator {
    public:
        iterator() = default;
        json operator*() const;
        iterator& operator++();
        bool operator!=(const iterator& o) const { return !(*this == o); }
        bool operator==(const iterator& o) const {
            // Two iterators at end() compare equal regardless of idx_.
            if (done_ && o.done_) return parent_ == o.parent_;
            return parent_ == o.parent_ && idx_ == o.idx_ && done_ == o.done_;
        }

        // For .items() structured binding support
        std::string key() const;
        json value() const;

    private:
        friend class json;
        std::shared_ptr<yyjson_mut_doc> doc_;
        yyjson_mut_val* parent_ = nullptr;
        std::vector<yyjson_mut_val*> keys_;   // populated for objects
        std::vector<yyjson_mut_val*> vals_;   // populated for both
        size_t idx_ = 0;
        bool is_object_ = false;
        bool done_ = true;
    };

    iterator begin() const;
    iterator end() const;

    // find (for .find(key) != .end() idiom)
    iterator find(const std::string& key) const;
    iterator find(const char* key) const;

    // items() for structured binding iteration — defined after class body.
    class items_proxy;
    items_proxy items() const;

    // ----- Internal helpers (public for ADL callers) -----
    yyjson_mut_doc* raw_doc() const { return doc_.get(); }
    yyjson_mut_val* raw_val() const { return val_; }

private:
    // Doc shared via custom deleter; val_ points into doc_.
    std::shared_ptr<yyjson_mut_doc> doc_;
    yyjson_mut_val* val_ = nullptr;

    // Write-through target (set only for handles returned by operator[]).
    // When operator= is called on an rvalue with a parent set, the assignment
    // replaces the corresponding key/index in the parent container.
    yyjson_mut_val* parent_ = nullptr;
    bool parent_is_array_ = false;
    std::string parent_key_;
    size_t parent_idx_ = 0;

    // ----- Private constructors -----
    json(std::shared_ptr<yyjson_mut_doc> doc, yyjson_mut_val* val);

    // Private ctor used by operator[] to create a "reference" handle.
    struct ref_tag {};
    json(ref_tag,
         std::shared_ptr<yyjson_mut_doc> doc,
         yyjson_mut_val* val,
         yyjson_mut_val* parent,
         std::string key);
    json(ref_tag,
         std::shared_ptr<yyjson_mut_doc> doc,
         yyjson_mut_val* val,
         yyjson_mut_val* parent,
         size_t idx);

    // Helper: ensure this handle has its own doc and root val.
    void ensure_own_doc();

    // Helper: write a value into parent at the stored key/idx.
    // src_val is a mut_val owned by some doc; we deep-copy into parent's doc.
    void write_through(yyjson_mut_val* src_val, yyjson_mut_doc* src_doc);

    // Helper: take a plain mut_val and install it as this json's own root.
    void reset_to(yyjson_mut_val* v);

    // Factory helpers
    static std::shared_ptr<yyjson_mut_doc> make_doc();

    friend void to_json_forwarding(json& j, const json& v);
};

// ----- items() proxy (defined outside json so it can hold a json member) -----
class json::items_proxy {
public:
    explicit items_proxy(const json& j) : j_(j) {}
    class iterator {
    public:
        iterator() = default;
        std::pair<std::string, json> operator*() const {
            return {it_.key(), it_.value()};
        }
        iterator& operator++() { ++it_; return *this; }
        bool operator!=(const iterator& o) const { return it_ != o.it_; }
        bool operator==(const iterator& o) const { return it_ == o.it_; }
    private:
        friend class items_proxy;
        json::iterator it_;
    };
    iterator begin() const { iterator it; it.it_ = j_.begin(); return it; }
    iterator end()   const { iterator it; it.it_ = j_.end();   return it; }
private:
    json j_;
};

inline json::items_proxy json::items() const { return items_proxy{*this}; }

// ----- ADL-style helpers used in types.h — extensible via argument-dependent lookup -----
inline void to_json(json& j, const json& v) { j = v; }
inline void from_json(const json& j, json& v) { v = j; }

// ----- Stream output (via dump()) -----
inline std::ostream& operator<<(std::ostream& os, const json& j) {
    return os << j.dump();
}

// ===========================================================================
// Explicit specializations declared here so template `value<T>()` sees them.
// ===========================================================================

template <> json        json::get<json>() const;
template <> std::string json::get<std::string>() const;
template <> bool        json::get<bool>() const;
template <> int         json::get<int>() const;
template <> unsigned    json::get<unsigned>() const;
template <> long        json::get<long>() const;
template <> unsigned long json::get<unsigned long>() const;
template <> long long   json::get<long long>() const;
template <> unsigned long long json::get<unsigned long long>() const;
template <> double      json::get<double>() const;
template <> float       json::get<float>() const;
template <> std::vector<std::string> json::get<std::vector<std::string>>() const;

template <typename T>
T json::value(const std::string& key, const T& default_val) const {
    if (!is_object()) return default_val;
    auto child = (*this)[key];
    if (child.is_null()) return default_val;
    try {
        return child.get<T>();
    } catch (...) {
        return default_val;
    }
}

inline std::string json::value(const std::string& key, const char* default_val) const {
    return value<std::string>(key, std::string(default_val));
}

} // namespace neograph
