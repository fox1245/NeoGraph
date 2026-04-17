#include <neograph/json.h>

#include <yyjson.h>

#include <algorithm>
#include <cstring>
#include <istream>
#include <sstream>

namespace neograph {

// ===========================================================================
// Helpers
// ===========================================================================

std::shared_ptr<yyjson_mut_doc> json::make_doc() {
    return std::shared_ptr<yyjson_mut_doc>(
        yyjson_mut_doc_new(nullptr),
        [](yyjson_mut_doc* d) { if (d) yyjson_mut_doc_free(d); });
}

void json::reset_to(yyjson_mut_val* v) {
    val_ = v;
    if (doc_) yyjson_mut_doc_set_root(doc_.get(), v);
}

void json::ensure_own_doc() {
    if (!doc_) {
        doc_ = make_doc();
        val_ = yyjson_mut_null(doc_.get());
        yyjson_mut_doc_set_root(doc_.get(), val_);
    }
}

// ===========================================================================
// Private constructors
// ===========================================================================

json::json(std::shared_ptr<yyjson_mut_doc> doc, yyjson_mut_val* val)
    : doc_(std::move(doc)), val_(val) {}

json::json(ref_tag,
           std::shared_ptr<yyjson_mut_doc> doc,
           yyjson_mut_val* val,
           yyjson_mut_val* parent,
           std::string key)
    : doc_(std::move(doc))
    , val_(val)
    , parent_(parent)
    , parent_is_array_(false)
    , parent_key_(std::move(key)) {}

json::json(ref_tag,
           std::shared_ptr<yyjson_mut_doc> doc,
           yyjson_mut_val* val,
           yyjson_mut_val* parent,
           size_t idx)
    : doc_(std::move(doc))
    , val_(val)
    , parent_(parent)
    , parent_is_array_(true)
    , parent_idx_(idx) {}

// ===========================================================================
// Public constructors
// ===========================================================================

json::json() : doc_(make_doc()) {
    val_ = yyjson_mut_null(doc_.get());
    yyjson_mut_doc_set_root(doc_.get(), val_);
}

json::json(std::nullptr_t) : json() {}

json::json(bool b) : doc_(make_doc()) {
    val_ = yyjson_mut_bool(doc_.get(), b);
    yyjson_mut_doc_set_root(doc_.get(), val_);
}

json::json(int i)           : json(static_cast<long long>(i)) {}
json::json(unsigned i)      : json(static_cast<unsigned long long>(i)) {}
json::json(long i)          : json(static_cast<long long>(i)) {}
json::json(unsigned long i) : json(static_cast<unsigned long long>(i)) {}

json::json(long long i) : doc_(make_doc()) {
    val_ = yyjson_mut_sint(doc_.get(), i);
    yyjson_mut_doc_set_root(doc_.get(), val_);
}

json::json(unsigned long long i) : doc_(make_doc()) {
    val_ = yyjson_mut_uint(doc_.get(), i);
    yyjson_mut_doc_set_root(doc_.get(), val_);
}

json::json(double d) : doc_(make_doc()) {
    val_ = yyjson_mut_real(doc_.get(), d);
    yyjson_mut_doc_set_root(doc_.get(), val_);
}

json::json(float f) : json(static_cast<double>(f)) {}

json::json(const char* s) : doc_(make_doc()) {
    val_ = yyjson_mut_strcpy(doc_.get(), s ? s : "");
    yyjson_mut_doc_set_root(doc_.get(), val_);
}

json::json(const std::string& s) : doc_(make_doc()) {
    val_ = yyjson_mut_strncpy(doc_.get(), s.data(), s.size());
    yyjson_mut_doc_set_root(doc_.get(), val_);
}

json::json(std::string_view s) : doc_(make_doc()) {
    val_ = yyjson_mut_strncpy(doc_.get(), s.data(), s.size());
    yyjson_mut_doc_set_root(doc_.get(), val_);
}

json::json(const std::vector<std::string>& v) : doc_(make_doc()) {
    val_ = yyjson_mut_arr(doc_.get());
    yyjson_mut_doc_set_root(doc_.get(), val_);
    for (const auto& s : v) {
        yyjson_mut_arr_append(val_,
            yyjson_mut_strncpy(doc_.get(), s.data(), s.size()));
    }
}

// Initializer-list: nlohmann-style heuristic.
// If every element is a 2-element array with a string first element, it's an object.
// Otherwise it's an array.
json::json(std::initializer_list<json> il) : doc_(make_doc()) {
    bool looks_like_object = il.size() > 0;
    for (const auto& el : il) {
        if (!el.is_array() || el.size() != 2) { looks_like_object = false; break; }
        auto first = el[size_t(0)];
        if (!first.is_string()) { looks_like_object = false; break; }
    }

    if (looks_like_object) {
        val_ = yyjson_mut_obj(doc_.get());
        yyjson_mut_doc_set_root(doc_.get(), val_);
        for (const auto& el : il) {
            std::string key = el[size_t(0)].get<std::string>();
            auto second = el[size_t(1)];
            yyjson_mut_val* v = yyjson_mut_val_mut_copy(doc_.get(), second.val_);
            yyjson_mut_obj_add(val_,
                yyjson_mut_strncpy(doc_.get(), key.data(), key.size()),
                v);
        }
    } else {
        val_ = yyjson_mut_arr(doc_.get());
        yyjson_mut_doc_set_root(doc_.get(), val_);
        for (const auto& el : il) {
            yyjson_mut_val* v = yyjson_mut_val_mut_copy(doc_.get(), el.val_);
            yyjson_mut_arr_append(val_, v);
        }
    }
}

// ===========================================================================
// Copy / Move
// ===========================================================================

json::json(const json& other) : doc_(make_doc()) {
    if (other.val_) {
        val_ = yyjson_mut_val_mut_copy(doc_.get(), other.val_);
    } else {
        val_ = yyjson_mut_null(doc_.get());
    }
    yyjson_mut_doc_set_root(doc_.get(), val_);
    // Do NOT propagate parent info — copy yields independent value.
}

json::json(json&& other) noexcept
    : doc_(std::move(other.doc_))
    , val_(other.val_)
    , parent_(other.parent_)
    , parent_is_array_(other.parent_is_array_)
    , parent_key_(std::move(other.parent_key_))
    , parent_idx_(other.parent_idx_)
{
    other.val_ = nullptr;
    other.parent_ = nullptr;
}

json& json::operator=(const json& other) & {
    if (this == &other) return *this;
    if (parent_) {
        write_through(other.val_, other.doc_.get());
        return *this;
    }
    doc_ = make_doc();
    if (other.val_) {
        val_ = yyjson_mut_val_mut_copy(doc_.get(), other.val_);
    } else {
        val_ = yyjson_mut_null(doc_.get());
    }
    yyjson_mut_doc_set_root(doc_.get(), val_);
    return *this;
}

json& json::operator=(json&& other) & noexcept {
    if (this == &other) return *this;
    if (parent_) {
        write_through(other.val_, other.doc_.get());
        return *this;
    }
    doc_ = std::move(other.doc_);
    val_ = other.val_;
    other.val_ = nullptr;
    return *this;
}

json& json::operator=(const json& other) && {
    if (parent_) {
        write_through(other.val_, other.doc_.get());
    } else {
        doc_ = make_doc();
        if (other.val_) {
            val_ = yyjson_mut_val_mut_copy(doc_.get(), other.val_);
        } else {
            val_ = yyjson_mut_null(doc_.get());
        }
        yyjson_mut_doc_set_root(doc_.get(), val_);
    }
    return *this;
}

json& json::operator=(json&& other) && noexcept {
    if (parent_) {
        write_through(other.val_, other.doc_.get());
    } else {
        doc_ = std::move(other.doc_);
        val_ = other.val_;
        other.val_ = nullptr;
    }
    return *this;
}

json::~json() = default;

void json::write_through(yyjson_mut_val* src_val, yyjson_mut_doc* /*src_doc*/) {
    if (!parent_ || !doc_) return;
    yyjson_mut_val* copied = src_val
        ? yyjson_mut_val_mut_copy(doc_.get(), src_val)
        : yyjson_mut_null(doc_.get());

    if (parent_is_array_) {
        yyjson_mut_arr_replace(parent_, parent_idx_, copied);
    } else {
        yyjson_mut_val* key = yyjson_mut_strncpy(doc_.get(),
            parent_key_.data(), parent_key_.size());
        yyjson_mut_obj_put(parent_, key, copied);
    }
    val_ = copied;
}

// ===========================================================================
// Factories
// ===========================================================================

json json::object() {
    auto doc = make_doc();
    yyjson_mut_val* v = yyjson_mut_obj(doc.get());
    yyjson_mut_doc_set_root(doc.get(), v);
    return json(std::move(doc), v);
}

json json::array() {
    auto doc = make_doc();
    yyjson_mut_val* v = yyjson_mut_arr(doc.get());
    yyjson_mut_doc_set_root(doc.get(), v);
    return json(std::move(doc), v);
}

json json::array(std::initializer_list<json> il) {
    auto doc = make_doc();
    yyjson_mut_val* v = yyjson_mut_arr(doc.get());
    yyjson_mut_doc_set_root(doc.get(), v);
    for (const auto& el : il) {
        yyjson_mut_arr_append(v, yyjson_mut_val_mut_copy(doc.get(), el.val_));
    }
    return json(std::move(doc), v);
}

// ===========================================================================
// Parse
// ===========================================================================

json json::parse(std::string_view s) {
    yyjson_read_err err;
    yyjson_doc* immut = yyjson_read_opts(
        const_cast<char*>(s.data()),
        s.size(),
        YYJSON_READ_NOFLAG,
        nullptr,
        &err);
    if (!immut) {
        throw parse_error(std::string("json::parse: ") + err.msg);
    }
    yyjson_mut_doc* mdoc = yyjson_doc_mut_copy(immut, nullptr);
    yyjson_doc_free(immut);
    if (!mdoc) {
        throw parse_error("json::parse: failed to create mutable copy");
    }
    yyjson_mut_val* root = yyjson_mut_doc_get_root(mdoc);
    std::shared_ptr<yyjson_mut_doc> sp(
        mdoc,
        [](yyjson_mut_doc* d) { if (d) yyjson_mut_doc_free(d); });
    return json(std::move(sp), root);
}

json json::parse(std::istream& in) {
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse(ss.str());
}

// ===========================================================================
// Type queries
// ===========================================================================

bool json::is_null() const noexcept {
    return !val_ || yyjson_mut_is_null(val_);
}
bool json::is_boolean() const noexcept {
    return val_ && yyjson_mut_is_bool(val_);
}
bool json::is_number() const noexcept {
    return val_ && yyjson_mut_is_num(val_);
}
bool json::is_number_integer() const noexcept {
    return val_ && (yyjson_mut_is_sint(val_) || yyjson_mut_is_uint(val_));
}
bool json::is_number_unsigned() const noexcept {
    return val_ && yyjson_mut_is_uint(val_);
}
bool json::is_number_float() const noexcept {
    return val_ && yyjson_mut_is_real(val_);
}
bool json::is_string() const noexcept {
    return val_ && yyjson_mut_is_str(val_);
}
bool json::is_array() const noexcept {
    return val_ && yyjson_mut_is_arr(val_);
}
bool json::is_object() const noexcept {
    return val_ && yyjson_mut_is_obj(val_);
}
bool json::is_primitive() const noexcept {
    return !is_null() && !is_array() && !is_object();
}

// ===========================================================================
// Size
// ===========================================================================

size_t json::size() const noexcept {
    if (!val_) return 0;
    if (yyjson_mut_is_arr(val_)) return yyjson_mut_arr_size(val_);
    if (yyjson_mut_is_obj(val_)) return yyjson_mut_obj_size(val_);
    if (yyjson_mut_is_str(val_)) return yyjson_mut_get_len(val_);
    return 0;
}

bool json::empty() const noexcept {
    return size() == 0;
}

// ===========================================================================
// Access: mutable (get-or-create)
// ===========================================================================

json json::operator[](const char* key) {
    return (*this)[std::string(key ? key : "")];
}

json json::operator[](const std::string& key) {
    ensure_own_doc();
    // Promote null → object if needed
    if (yyjson_mut_is_null(val_)) {
        yyjson_mut_val* newobj = yyjson_mut_obj(doc_.get());
        if (parent_) {
            // We're a ref — replace parent's slot
            if (parent_is_array_) {
                yyjson_mut_arr_replace(parent_, parent_idx_, newobj);
            } else {
                yyjson_mut_val* k = yyjson_mut_strncpy(doc_.get(),
                    parent_key_.data(), parent_key_.size());
                yyjson_mut_obj_put(parent_, k, newobj);
            }
        } else {
            yyjson_mut_doc_set_root(doc_.get(), newobj);
        }
        val_ = newobj;
    }
    if (!yyjson_mut_is_obj(val_)) {
        // Type mismatch — return null ref (read-only behavior)
        return json(ref_tag{}, doc_, nullptr, val_, key);
    }
    yyjson_mut_val* child = yyjson_mut_obj_getn(val_, key.data(), key.size());
    if (!child) {
        child = yyjson_mut_null(doc_.get());
        yyjson_mut_obj_add(val_,
            yyjson_mut_strncpy(doc_.get(), key.data(), key.size()),
            child);
    }
    return json(ref_tag{}, doc_, child, val_, key);
}

json json::operator[](size_t idx) {
    ensure_own_doc();
    if (!yyjson_mut_is_arr(val_)) {
        return json(ref_tag{}, doc_, nullptr, val_, idx);
    }
    if (idx >= yyjson_mut_arr_size(val_)) {
        return json(ref_tag{}, doc_, nullptr, val_, idx);
    }
    yyjson_mut_val* child = yyjson_mut_arr_get(val_, idx);
    return json(ref_tag{}, doc_, child, val_, idx);
}

// ===========================================================================
// Access: const (read-only, no creation)
// ===========================================================================

json json::operator[](const char* key) const {
    return (*this)[std::string(key ? key : "")];
}

json json::operator[](const std::string& key) const {
    if (!val_ || !yyjson_mut_is_obj(val_)) return json();
    yyjson_mut_val* child = yyjson_mut_obj_getn(val_, key.data(), key.size());
    if (!child) return json();
    return json(doc_, child);
}

json json::operator[](size_t idx) const {
    if (!val_ || !yyjson_mut_is_arr(val_)) return json();
    if (idx >= yyjson_mut_arr_size(val_)) return json();
    yyjson_mut_val* child = yyjson_mut_arr_get(val_, idx);
    return json(doc_, child);
}

// ===========================================================================
// at (throwing)
// ===========================================================================

json json::at(const char* key) { return at(std::string(key ? key : "")); }
json json::at(const std::string& key) {
    if (!val_ || !yyjson_mut_is_obj(val_))
        throw type_error("json::at: not an object");
    yyjson_mut_val* child = yyjson_mut_obj_getn(val_, key.data(), key.size());
    if (!child) throw out_of_range("json::at: key not found: " + key);
    return json(ref_tag{}, doc_, child, val_, key);
}
json json::at(size_t idx) {
    if (!val_ || !yyjson_mut_is_arr(val_))
        throw type_error("json::at: not an array");
    if (idx >= yyjson_mut_arr_size(val_))
        throw out_of_range("json::at: index out of range");
    yyjson_mut_val* child = yyjson_mut_arr_get(val_, idx);
    return json(ref_tag{}, doc_, child, val_, idx);
}

json json::at(const char* key) const { return at(std::string(key ? key : "")); }
json json::at(const std::string& key) const {
    if (!val_ || !yyjson_mut_is_obj(val_))
        throw type_error("json::at: not an object");
    yyjson_mut_val* child = yyjson_mut_obj_getn(val_, key.data(), key.size());
    if (!child) throw out_of_range("json::at: key not found: " + key);
    return json(doc_, child);
}
json json::at(size_t idx) const {
    if (!val_ || !yyjson_mut_is_arr(val_))
        throw type_error("json::at: not an array");
    if (idx >= yyjson_mut_arr_size(val_))
        throw out_of_range("json::at: index out of range");
    yyjson_mut_val* child = yyjson_mut_arr_get(val_, idx);
    return json(doc_, child);
}

// ===========================================================================
// Membership
// ===========================================================================

bool json::contains(const char* key) const {
    return contains(std::string(key ? key : ""));
}
bool json::contains(const std::string& key) const {
    if (!val_ || !yyjson_mut_is_obj(val_)) return false;
    return yyjson_mut_obj_getn(val_, key.data(), key.size()) != nullptr;
}

// ===========================================================================
// Push back (array)
// ===========================================================================

void json::push_back(const json& v) {
    ensure_own_doc();
    if (yyjson_mut_is_null(val_)) {
        yyjson_mut_val* newarr = yyjson_mut_arr(doc_.get());
        if (parent_) {
            if (parent_is_array_) {
                yyjson_mut_arr_replace(parent_, parent_idx_, newarr);
            } else {
                yyjson_mut_val* k = yyjson_mut_strncpy(doc_.get(),
                    parent_key_.data(), parent_key_.size());
                yyjson_mut_obj_put(parent_, k, newarr);
            }
        } else {
            yyjson_mut_doc_set_root(doc_.get(), newarr);
        }
        val_ = newarr;
    }
    if (!yyjson_mut_is_arr(val_)) {
        throw type_error("json::push_back: not an array");
    }
    yyjson_mut_val* copied = v.val_
        ? yyjson_mut_val_mut_copy(doc_.get(), v.val_)
        : yyjson_mut_null(doc_.get());
    yyjson_mut_arr_append(val_, copied);
}

void json::push_back(json&& v) {
    push_back(static_cast<const json&>(v));
}

// ===========================================================================
// Serialization
// ===========================================================================

std::string json::dump(int indent) const {
    if (!val_) return "null";
    // Create a temp doc pointing at val_ for writing (or serialize direct)
    yyjson_write_flag flag = YYJSON_WRITE_NOFLAG;
    if (indent > 0) flag |= YYJSON_WRITE_PRETTY;

    size_t len = 0;
    char* raw = yyjson_mut_val_write(val_, flag, &len);
    if (!raw) return "";
    std::string result(raw, len);
    free(raw);
    return result;
}

// ===========================================================================
// Equality
// ===========================================================================

bool json::operator==(const json& other) const {
    // Compare by canonical serialization.
    return dump() == other.dump();
}

// ===========================================================================
// Iteration
// ===========================================================================

json::iterator json::begin() const {
    iterator it;
    it.doc_ = doc_;
    it.parent_ = val_;
    if (!val_) { it.done_ = true; return it; }
    if (yyjson_mut_is_arr(val_)) {
        it.is_object_ = false;
        size_t n = yyjson_mut_arr_size(val_);
        it.vals_.reserve(n);
        yyjson_mut_arr_iter ai;
        yyjson_mut_arr_iter_init(val_, &ai);
        yyjson_mut_val* v;
        while ((v = yyjson_mut_arr_iter_next(&ai))) it.vals_.push_back(v);
        it.done_ = it.vals_.empty();
    } else if (yyjson_mut_is_obj(val_)) {
        it.is_object_ = true;
        size_t n = yyjson_mut_obj_size(val_);
        it.keys_.reserve(n);
        it.vals_.reserve(n);
        yyjson_mut_obj_iter oi;
        yyjson_mut_obj_iter_init(val_, &oi);
        yyjson_mut_val* k;
        while ((k = yyjson_mut_obj_iter_next(&oi))) {
            it.keys_.push_back(k);
            it.vals_.push_back(yyjson_mut_obj_iter_get_val(k));
        }
        it.done_ = it.keys_.empty();
    } else {
        it.done_ = true;
    }
    it.idx_ = 0;
    return it;
}

json::iterator json::end() const {
    iterator it;
    it.doc_ = doc_;
    it.parent_ = val_;
    it.done_ = true;
    if (val_ && yyjson_mut_is_obj(val_)) it.is_object_ = true;
    return it;
}

json json::iterator::operator*() const {
    if (done_ || idx_ >= vals_.size()) return json();
    return json(doc_, vals_[idx_]);
}

json::iterator& json::iterator::operator++() {
    if (done_) return *this;
    ++idx_;
    if (idx_ >= vals_.size()) done_ = true;
    return *this;
}

std::string json::iterator::key() const {
    if (!is_object_ || done_ || idx_ >= keys_.size()) return "";
    yyjson_mut_val* k = keys_[idx_];
    const char* s = yyjson_mut_get_str(k);
    return s ? std::string(s, yyjson_mut_get_len(k)) : std::string();
}

json json::iterator::value() const {
    if (done_ || idx_ >= vals_.size()) return json();
    return json(doc_, vals_[idx_]);
}

// ===========================================================================
// find
// ===========================================================================

json::iterator json::find(const std::string& key) const {
    iterator it = begin();
    if (!it.is_object_) return end();
    while (!it.done_) {
        if (it.key() == key) return it;
        ++it;
    }
    return end();
}

json::iterator json::find(const char* key) const {
    return find(std::string(key ? key : ""));
}

// ===========================================================================
// get<T>() specializations
// ===========================================================================

template <>
json json::get<json>() const {
    return *this;
}

template <>
std::string json::get<std::string>() const {
    if (!val_ || !yyjson_mut_is_str(val_)) {
        throw type_error("json::get<string>: not a string");
    }
    const char* s = yyjson_mut_get_str(val_);
    size_t len = yyjson_mut_get_len(val_);
    return s ? std::string(s, len) : std::string();
}

template <>
bool json::get<bool>() const {
    if (!val_ || !yyjson_mut_is_bool(val_))
        throw type_error("json::get<bool>: not a bool");
    return yyjson_mut_get_bool(val_);
}

template <>
int json::get<int>() const {
    if (!val_) throw type_error("json::get<int>: null");
    if (yyjson_mut_is_sint(val_)) return static_cast<int>(yyjson_mut_get_sint(val_));
    if (yyjson_mut_is_uint(val_)) return static_cast<int>(yyjson_mut_get_uint(val_));
    if (yyjson_mut_is_real(val_)) return static_cast<int>(yyjson_mut_get_real(val_));
    throw type_error("json::get<int>: not a number");
}

template <>
unsigned json::get<unsigned>() const {
    return static_cast<unsigned>(get<long long>());
}

template <>
long json::get<long>() const {
    return static_cast<long>(get<long long>());
}

template <>
unsigned long json::get<unsigned long>() const {
    return static_cast<unsigned long>(get<unsigned long long>());
}

template <>
long long json::get<long long>() const {
    if (!val_) throw type_error("json::get<long long>: null");
    if (yyjson_mut_is_sint(val_)) return yyjson_mut_get_sint(val_);
    if (yyjson_mut_is_uint(val_)) return static_cast<long long>(yyjson_mut_get_uint(val_));
    if (yyjson_mut_is_real(val_)) return static_cast<long long>(yyjson_mut_get_real(val_));
    throw type_error("json::get<long long>: not a number");
}

template <>
unsigned long long json::get<unsigned long long>() const {
    if (!val_) throw type_error("json::get<unsigned long long>: null");
    if (yyjson_mut_is_uint(val_)) return yyjson_mut_get_uint(val_);
    if (yyjson_mut_is_sint(val_)) return static_cast<unsigned long long>(yyjson_mut_get_sint(val_));
    if (yyjson_mut_is_real(val_)) return static_cast<unsigned long long>(yyjson_mut_get_real(val_));
    throw type_error("json::get<unsigned long long>: not a number");
}

template <>
double json::get<double>() const {
    if (!val_) throw type_error("json::get<double>: null");
    if (yyjson_mut_is_real(val_)) return yyjson_mut_get_real(val_);
    if (yyjson_mut_is_sint(val_)) return static_cast<double>(yyjson_mut_get_sint(val_));
    if (yyjson_mut_is_uint(val_)) return static_cast<double>(yyjson_mut_get_uint(val_));
    throw type_error("json::get<double>: not a number");
}

template <>
float json::get<float>() const {
    return static_cast<float>(get<double>());
}

template <>
std::vector<std::string> json::get<std::vector<std::string>>() const {
    if (!val_ || !yyjson_mut_is_arr(val_))
        throw type_error("json::get<vector<string>>: not an array");
    std::vector<std::string> out;
    out.reserve(yyjson_mut_arr_size(val_));
    yyjson_mut_arr_iter it;
    yyjson_mut_arr_iter_init(val_, &it);
    yyjson_mut_val* v;
    while ((v = yyjson_mut_arr_iter_next(&it))) {
        if (yyjson_mut_is_str(v)) {
            out.emplace_back(yyjson_mut_get_str(v), yyjson_mut_get_len(v));
        }
    }
    return out;
}

} // namespace neograph
