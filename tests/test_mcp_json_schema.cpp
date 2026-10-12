// JSON Schema 2020-12 validation shared by the MCP server, the MCP client and
// the Harness (MCP-S-03, MCP-C-16). MCP 2025-11-25 makes 2020-12 the default
// dialect of every tool schema; the official SDKs validate with full 2020-12
// validators, so NeoGraph's verdicts must match theirs.
#include <gtest/gtest.h>

#include <neograph/mcp/json_schema.h>

#include <stdexcept>
#include <string>

namespace {

using neograph::json;
using neograph::mcp::validate_json_schema;
using neograph::mcp::validate_json_value;

bool accepts(const json& schema, const json& value) {
    try {
        validate_json_value(value, schema, "value", "$");
        return true;
    } catch (const std::invalid_argument&) {
        return false;
    }
}

std::string rejection(const json& schema, const json& value) {
    try {
        validate_json_value(value, schema, "value", "$");
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
    return {};
}

TEST(McpJsonSchema, IntegerAcceptsAnIntegralNumber) {
    const json schema = {{"type", "integer"}};
    EXPECT_TRUE(accepts(schema, json::parse("1.0")));
    EXPECT_TRUE(accepts(schema, json::parse("-3")));
    EXPECT_TRUE(accepts(schema, json::parse("18446744073709551615")));
    EXPECT_FALSE(accepts(schema, json::parse("1.5")));
    EXPECT_FALSE(accepts(schema, json("1")));
}

TEST(McpJsonSchema, NumericBoundsAndMultipleOf) {
    const json schema = json::parse(R"({"type":"number","minimum":0,"exclusiveMaximum":10,
                                        "multipleOf":0.5})");
    EXPECT_TRUE(accepts(schema, json::parse("0")));
    EXPECT_TRUE(accepts(schema, json::parse("9.5")));
    EXPECT_FALSE(accepts(schema, json::parse("-1")));
    EXPECT_FALSE(accepts(schema, json::parse("10")));
    EXPECT_FALSE(accepts(schema, json::parse("2.25")));
    EXPECT_NE(rejection(schema, json::parse("-1")).find("value at $ is less than the minimum 0"),
              std::string::npos);
    EXPECT_FALSE(accepts(json::parse(R"({"multipleOf":3})"), json::parse("7")));
    EXPECT_TRUE(accepts(json::parse(R"({"multipleOf":3})"), json::parse("9")));
}

TEST(McpJsonSchema, StringsCountCodePointsAndMatchPatterns) {
    const json schema = json::parse(R"({"type":"string","minLength":2,"maxLength":3,
                                        "pattern":"^[a-z가-힣]+$"})");
    EXPECT_TRUE(accepts(schema, json("가나")));  // 2 code points, 6 bytes
    EXPECT_FALSE(accepts(schema, json("a")));
    EXPECT_FALSE(accepts(schema, json("abcd")));
    EXPECT_FALSE(accepts(schema, json("AB")));
    // A pattern is a search, not an anchored match.
    EXPECT_TRUE(accepts(json::parse(R"({"pattern":"b"})"), json("abc")));
    // An oversized subject fails its pattern instead of risking the matcher.
    EXPECT_FALSE(accepts(json::parse(R"({"pattern":"a"})"), json(std::string(20000, 'a'))));
}

TEST(McpJsonSchema, EqualityIsNumericAndOrderInsensitive) {
    EXPECT_TRUE(accepts(json::parse(R"({"const":1})"), json::parse("1.0")));
    EXPECT_TRUE(accepts(json::parse(R"({"enum":[{"a":1,"b":[2]}]})"),
                        json::parse(R"({"b":[2.0],"a":1})")));
    EXPECT_FALSE(accepts(json::parse(R"({"const":"1"})"), json::parse("1")));
    EXPECT_FALSE(accepts(json::parse(R"({"uniqueItems":true})"), json::parse("[1, 1.0]")));
    EXPECT_TRUE(accepts(json::parse(R"({"uniqueItems":true})"), json::parse("[1, \"1\"]")));
}

TEST(McpJsonSchema, PydanticDefsAndRefs) {
    // The shape pydantic and zod generate: definitions under $defs, $ref to them.
    const json schema = json::parse(R"({
        "$defs": {"Item": {"type": "object", "properties": {"name": {"type": "string"},
                                                             "qty": {"type": "integer", "minimum": 1}},
                           "required": ["name"], "additionalProperties": false}},
        "type": "object",
        "properties": {"items": {"type": "array", "items": {"$ref": "#/$defs/Item"}},
                       "note": {"anyOf": [{"type": "string"}, {"type": "null"}], "default": null}},
        "required": ["items"]})");
    EXPECT_NO_THROW(validate_json_schema(schema));
    EXPECT_TRUE(accepts(schema, json::parse(R"({"items":[{"name":"a","qty":2}],"note":null})")));
    EXPECT_FALSE(accepts(schema, json::parse(R"({"items":[{"name":"a","qty":0}]})")));
    EXPECT_FALSE(accepts(schema, json::parse(R"({"items":[{"qty":1}]})")));
    EXPECT_FALSE(accepts(schema, json::parse(R"({"items":[{"name":"a","extra":1}]})")));
    EXPECT_FALSE(accepts(schema, json::parse(R"({"items":[],"note":3})")));
    EXPECT_NE(rejection(schema, json::parse(R"({"items":[{"qty":1}]})"))
                  .find("value at $/items/0 is missing required property name"),
              std::string::npos);
}

TEST(McpJsonSchema, RecursiveRefsAnchorsAndPointerEscapes) {
    const json tree = json::parse(R"({
        "$defs": {"node": {"$anchor": "node", "type": "object",
                           "properties": {"children": {"type": "array", "items": {"$ref": "#node"}},
                                          "value": {"type": "integer"}}},
                  "a/b": {"type": "string"}},
        "$ref": "#/$defs/node",
        "properties": {"label": {"$ref": "#/$defs/a~1b"}}})");
    EXPECT_TRUE(accepts(tree, json::parse(
        R"({"value":1,"label":"x","children":[{"value":2,"children":[{"value":3}]}]})")));
    EXPECT_FALSE(accepts(tree, json::parse(R"({"children":[{"children":[{"value":"x"}]}]})")));
    EXPECT_FALSE(accepts(tree, json::parse(R"({"label":7})")));
}

TEST(McpJsonSchema, CompositionKeywords) {
    const json schema = json::parse(R"({
        "oneOf": [{"type": "integer", "multipleOf": 2}, {"type": "integer", "multipleOf": 3}],
        "not": {"const": 12}})");
    EXPECT_TRUE(accepts(schema, json::parse("4")));
    EXPECT_TRUE(accepts(schema, json::parse("9")));
    EXPECT_FALSE(accepts(schema, json::parse("6")));   // both branches
    EXPECT_FALSE(accepts(schema, json::parse("7")));   // neither branch
    EXPECT_FALSE(accepts(schema, json::parse("12")));  // not

    const json conditional = json::parse(R"({
        "type": "object",
        "if": {"properties": {"kind": {"const": "file"}}, "required": ["kind"]},
        "then": {"required": ["path"]},
        "else": {"required": ["url"]}})");
    EXPECT_TRUE(accepts(conditional, json::parse(R"({"kind":"file","path":"/x"})")));
    EXPECT_FALSE(accepts(conditional, json::parse(R"({"kind":"file","url":"u"})")));
    EXPECT_TRUE(accepts(conditional, json::parse(R"({"url":"u"})")));
}

TEST(McpJsonSchema, ObjectKeywords) {
    const json schema = json::parse(R"({
        "type": "object",
        "properties": {"a": {"type": "integer"}},
        "patternProperties": {"^x-": {"type": "string"}},
        "additionalProperties": false,
        "propertyNames": {"maxLength": 5},
        "dependentRequired": {"a": ["x-b"]},
        "dependentSchemas": {"x-c": {"required": ["a"]}},
        "minProperties": 1, "maxProperties": 3})");
    EXPECT_TRUE(accepts(schema, json::parse(R"({"a":1,"x-b":"s"})")));
    EXPECT_FALSE(accepts(schema, json::parse(R"({"a":1})")));            // dependentRequired
    EXPECT_FALSE(accepts(schema, json::parse(R"({"x-c":"s"})")));        // dependentSchemas
    EXPECT_FALSE(accepts(schema, json::parse(R"({"other":1})")));        // additionalProperties
    EXPECT_FALSE(accepts(schema, json::parse(R"({"x-long1":"s"})")));    // propertyNames
    EXPECT_FALSE(accepts(schema, json::parse(R"({})")));                 // minProperties
    EXPECT_FALSE(accepts(schema, json::parse(R"({"x-b":1})")));          // patternProperties
    EXPECT_NE(rejection(schema, json::parse(R"({"other":1})")).find("has unexpected property other"),
              std::string::npos);
}

TEST(McpJsonSchema, UnevaluatedPropertiesSeeThroughApplicators) {
    const json schema = json::parse(R"({
        "allOf": [{"properties": {"a": {"type": "string"}}}],
        "anyOf": [{"properties": {"b": {"type": "integer"}}, "required": ["b"]},
                  {"properties": {"c": {"type": "boolean"}}, "required": ["c"]}],
        "unevaluatedProperties": false})");
    EXPECT_TRUE(accepts(schema, json::parse(R"({"a":"x","b":1})")));
    EXPECT_TRUE(accepts(schema, json::parse(R"({"c":true})")));
    EXPECT_FALSE(accepts(schema, json::parse(R"({"b":1,"d":0})")));
    // A failed branch evaluates nothing: "c" is not evaluated when its branch fails.
    EXPECT_FALSE(accepts(schema, json::parse(R"({"b":1,"c":"no"})")));
}

TEST(McpJsonSchema, ArrayKeywords) {
    const json tuple = json::parse(R"({"prefixItems":[{"type":"string"},{"type":"integer"}],
                                       "items":false})");
    EXPECT_TRUE(accepts(tuple, json::parse(R"(["a",1])")));
    EXPECT_FALSE(accepts(tuple, json::parse(R"(["a",1,2])")));
    EXPECT_FALSE(accepts(tuple, json::parse(R"([1,"a"])")));

    const json contains = json::parse(R"({"contains":{"type":"integer"},"minContains":2,
                                          "maxContains":3,"minItems":2})");
    EXPECT_TRUE(accepts(contains, json::parse(R"([1,"a",2])")));
    EXPECT_FALSE(accepts(contains, json::parse(R"([1,"a"])")));
    EXPECT_FALSE(accepts(contains, json::parse(R"([1,2,3,4])")));

    const json unevaluated = json::parse(R"({"prefixItems":[{"type":"string"}],
                                             "contains":{"type":"integer"},
                                             "unevaluatedItems":false})");
    EXPECT_TRUE(accepts(unevaluated, json::parse(R"(["a",1,2])")));
    EXPECT_FALSE(accepts(unevaluated, json::parse(R"(["a",1,true])")));
}

TEST(McpJsonSchema, BooleanSchemas) {
    EXPECT_TRUE(accepts(json(true), json::parse(R"({"anything":[1]})")));
    EXPECT_FALSE(accepts(json(false), json::parse("null")));
    const json schema = json::parse(R"({"type":"object","properties":{"forbidden":false}})");
    EXPECT_TRUE(accepts(schema, json::parse(R"({"ok":1})")));
    EXPECT_FALSE(accepts(schema, json::parse(R"({"forbidden":1})")));
}

TEST(McpJsonSchema, UnsupportedDialectsAndKeywordsAreRefused) {
    EXPECT_NO_THROW(validate_json_schema(json::parse(
        R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object"})")));
    EXPECT_THROW(validate_json_schema(json::parse(
        R"({"$schema":"http://json-schema.org/draft-07/schema#","type":"object"})")),
        std::invalid_argument);
    EXPECT_THROW(validate_json_schema(json::parse(R"({"$dynamicRef":"#meta"})")),
                 std::invalid_argument);
    EXPECT_THROW(validate_json_schema(json::parse(R"({"$ref":"https://example.com/other.json"})")),
                 std::invalid_argument);
    EXPECT_THROW(validate_json_schema(json::parse(R"({"$ref":"#/$defs/missing"})")),
                 std::invalid_argument);
    EXPECT_THROW(validate_json_schema(json::parse(R"({"items":[{"type":"string"}]})")),
                 std::invalid_argument);
    EXPECT_THROW(validate_json_schema(json::parse(R"({"type":"imaginary"})")),
                 std::invalid_argument);
    EXPECT_THROW(validate_json_schema(json::parse(R"({"minimum":"0"})")), std::invalid_argument);
    EXPECT_THROW(validate_json_schema(json::parse(R"({"pattern":"(unclosed"})")),
                 std::invalid_argument);
    EXPECT_THROW(validate_json_schema(json::parse(R"({"anyOf":[]})")), std::invalid_argument);
    EXPECT_THROW(validate_json_schema(json::parse("3")), std::invalid_argument);
    // Unknown keywords are annotations in 2020-12, as format is by default.
    EXPECT_NO_THROW(validate_json_schema(json::parse(R"({"x-vendor":1,"format":"uri"})")));
    EXPECT_TRUE(accepts(json::parse(R"({"format":"email"})"), json("not an email")));
}

TEST(McpJsonSchema, SchemaNestingIsBounded) {
    json schema = {{"type", "object"}};
    for (int i = 0; i < 80; ++i) schema = json{{"properties", {{"x", schema}}}};
    EXPECT_THROW(validate_json_schema(schema), std::invalid_argument);
}

TEST(McpJsonSchema, EmbeddedResourcesResolveByTheirId) {
    const json schema = json::parse(R"({
        "$id": "https://example.com/root.json",
        "$defs": {"inner": {"$id": "https://example.com/inner.json",
                            "$defs": {"leaf": {"type": "integer"}},
                            "$ref": "#/$defs/leaf"}},
        "properties": {"n": {"$ref": "https://example.com/inner.json"}}})");
    EXPECT_NO_THROW(validate_json_schema(schema));
    EXPECT_TRUE(accepts(schema, json::parse(R"({"n":3})")));
    EXPECT_FALSE(accepts(schema, json::parse(R"({"n":"3"})")));
}

} // namespace
