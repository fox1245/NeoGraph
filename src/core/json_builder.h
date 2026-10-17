// Internal: builds one JSON value inside a single yyjson document (#478).
//
// json{{"key", value}, ...} makes a document for every key, every value and
// every pair, and copies each value again into the result: a Program record
// body made that way created thousands of documents per invocation. The
// builder makes each value once, in the document that it ends up in.
//
// make() has one overload for each converting constructor of json, with the
// same parameter types, so for any argument overload resolution picks the
// same conversion and a member gets the yyjson type json(argument) gives it.
// A tree built here therefore has the same canonical bytes as the tree the
// equivalent initializer list builds.
#pragma once

#include <neograph/api.h>
#include <neograph/json.h>

#include <yyjson.h>

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neograph::detail {

class NEOGRAPH_API JsonBuilder {
public:
    using Value  = yyjson_mut_val*;
    using Member = std::pair<std::string_view, Value>;

    JsonBuilder();
    JsonBuilder(const JsonBuilder&)            = delete;
    JsonBuilder& operator=(const JsonBuilder&) = delete;

    // The converting constructors of json, one for one.
    Value make(std::nullptr_t);
    Value make(bool value);
    Value make(int value);
    Value make(unsigned value);
    Value make(long value);
    Value make(unsigned long value);
    Value make(long long value);
    Value make(unsigned long long value);
    Value make(double value);
    Value make(float value);
    Value make(const char* value);
    Value make(const std::string& value);
    Value make(std::string_view value);
    Value make(const std::vector<std::string>& value);
    // A deep copy of an existing value, as json's initializer list makes.
    Value make(const json& value);

    Value null() { return make(nullptr); }

    // Members are added in order, without a duplicate check, as json's
    // initializer list adds them.
    Value object(std::initializer_list<Member> members = {});
    Value array(std::initializer_list<Value> items = {});
    void  push(Value array, Value item);
    // Replaces a present member or appends it, as `object[key] = value` does.
    void set(Value object, std::string_view key, Value value);

    // The json owning this document, with `root` as its value. The builder is
    // spent afterwards.
    json finish(Value root) &&;

private:
    std::shared_ptr<yyjson_mut_doc> doc_;
};

}  // namespace neograph::detail
