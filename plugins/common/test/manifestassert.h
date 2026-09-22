#pragma once

#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "testharness.h"

// Manifest and staged-package assertions shared by the native Control packages.
//
// The host loads a package from a directory that must contain exactly one
// plugin.json next to a package-relative command. These helpers read the staged
// copy the same way the loader does and then let each package assert its own
// fixed contract on top.
namespace ManifestAssert {

using TestHarness::check;

struct JsonValue {
    enum class Kind { String, Array, Object, Number, Boolean, Null };

    Kind kind = Kind::Null;
    std::string stringValue;
    std::vector<JsonValue> arrayValue;
    std::map<std::string, JsonValue> objectValue;
};

class JsonParser {
public:
    explicit JsonParser(std::string input) : m_input(std::move(input)) {}

    JsonValue parse()
    {
        const auto result = parseValue();
        skipWhitespace();
        if (m_position != m_input.size()) {
            throw std::runtime_error("unexpected trailing JSON input");
        }
        return result;
    }

private:
    JsonValue parseValue()
    {
        skipWhitespace();
        if (m_position == m_input.size()) {
            throw std::runtime_error("unexpected end of JSON input");
        }
        switch (m_input[m_position]) {
        case '"':
            return {JsonValue::Kind::String, parseString(), {}, {}};
        case '[':
            return parseArray();
        case '{':
            return parseObject();
        case 't':
            expectLiteral("true");
            return {JsonValue::Kind::Boolean, "true", {}, {}};
        case 'f':
            expectLiteral("false");
            return {JsonValue::Kind::Boolean, "false", {}, {}};
        case 'n':
            expectLiteral("null");
            return {JsonValue::Kind::Null, {}, {}, {}};
        default:
            return parseNumber();
        }
    }

    JsonValue parseArray()
    {
        expect('[');
        JsonValue result{JsonValue::Kind::Array, {}, {}, {}};
        skipWhitespace();
        if (consume(']')) {
            return result;
        }
        for (;;) {
            result.arrayValue.push_back(parseValue());
            skipWhitespace();
            if (consume(']')) {
                return result;
            }
            expect(',');
        }
    }

    JsonValue parseObject()
    {
        expect('{');
        JsonValue result{JsonValue::Kind::Object, {}, {}, {}};
        skipWhitespace();
        if (consume('}')) {
            return result;
        }
        for (;;) {
            skipWhitespace();
            if (m_position == m_input.size() || m_input[m_position] != '"') {
                throw std::runtime_error("expected object key");
            }
            const auto key = parseString();
            expect(':');
            result.objectValue.emplace(key, parseValue());
            skipWhitespace();
            if (consume('}')) {
                return result;
            }
            expect(',');
        }
    }

    JsonValue parseNumber()
    {
        const auto begin = m_position;
        while (m_position < m_input.size()
               && (std::isdigit(static_cast<unsigned char>(m_input[m_position]))
                   || m_input[m_position] == '-' || m_input[m_position] == '+'
                   || m_input[m_position] == '.' || m_input[m_position] == 'e'
                   || m_input[m_position] == 'E')) {
            ++m_position;
        }
        if (begin == m_position) {
            throw std::runtime_error("invalid JSON value");
        }
        return {JsonValue::Kind::Number,
                m_input.substr(begin, m_position - begin), {}, {}};
    }

    std::string parseString()
    {
        expect('"');
        std::string result;
        while (m_position < m_input.size()) {
            const auto character = m_input[m_position++];
            if (character == '"') {
                return result;
            }
            if (character != '\\') {
                result += character;
                continue;
            }
            if (m_position == m_input.size()) {
                throw std::runtime_error("incomplete JSON escape");
            }
            const auto escaped = m_input[m_position++];
            switch (escaped) {
            case '"':
                result += '"';
                break;
            case '\\':
                result += '\\';
                break;
            case '/':
                result += '/';
                break;
            case 'b':
                result += '\b';
                break;
            case 'f':
                result += '\f';
                break;
            case 'n':
                result += '\n';
                break;
            case 'r':
                result += '\r';
                break;
            case 't':
                result += '\t';
                break;
            default:
                throw std::runtime_error("unsupported JSON escape");
            }
        }
        throw std::runtime_error("unterminated JSON string");
    }

    void expect(const char expected)
    {
        skipWhitespace();
        if (m_position == m_input.size() || m_input[m_position] != expected) {
            throw std::runtime_error("unexpected JSON character");
        }
        ++m_position;
    }

    void expectLiteral(const char *expected)
    {
        while (*expected != '\0') {
            if (m_position == m_input.size()
                || m_input[m_position] != *expected) {
                throw std::runtime_error("unexpected JSON literal");
            }
            ++m_position;
            ++expected;
        }
    }

    bool consume(const char expected)
    {
        if (m_position < m_input.size() && m_input[m_position] == expected) {
            ++m_position;
            return true;
        }
        return false;
    }

    void skipWhitespace()
    {
        while (m_position < m_input.size()
               && std::isspace(
                   static_cast<unsigned char>(m_input[m_position]))) {
            ++m_position;
        }
    }

    std::string m_input;
    size_t m_position = 0;
};

inline const JsonValue *member(const JsonValue &object, const char *name)
{
    if (object.kind != JsonValue::Kind::Object) {
        return nullptr;
    }
    const auto found = object.objectValue.find(name);
    return found == object.objectValue.end() ? nullptr : &found->second;
}

inline bool isString(const JsonValue *value, const char *expected)
{
    return value && value->kind == JsonValue::Kind::String
           && value->stringValue == expected;
}

inline bool isExactStringArray(const JsonValue *value, const char *expected)
{
    return value && value->kind == JsonValue::Kind::Array
           && value->arrayValue.size() == 1
           && value->arrayValue.front().kind == JsonValue::Kind::String
           && value->arrayValue.front().stringValue == expected;
}

inline bool hasString(const JsonValue *value, const char *expected)
{
    if (!value || value->kind != JsonValue::Kind::Array) {
        return false;
    }
    for (const auto &element : value->arrayValue) {
        if (element.kind == JsonValue::Kind::String
            && element.stringValue == expected) {
            return true;
        }
    }
    return false;
}

inline bool isWithin(const std::filesystem::path &root,
                     const std::filesystem::path &path)
{
    auto rootPart = root.begin();
    auto pathPart = path.begin();
    while (rootPart != root.end()) {
        if (pathPart == path.end() || *rootPart != *pathPart) {
            return false;
        }
        ++rootPart;
        ++pathPart;
    }
    return true;
}

struct Package {
    std::string id;
    std::string name;
    std::string description;
    std::string version;
    std::string appMinVersion;
    std::string backend;
    std::string command;
    std::vector<std::string> capabilities;
    std::vector<std::string> extensions;
    std::vector<std::string> arguments;
    std::vector<long long> successExitCodes;
    long long timeoutMs = -1;
    bool hasTimeout = false;
    JsonValue root;
    std::filesystem::path packageRoot;
    std::filesystem::path manifestPath;
    std::filesystem::path commandPath;
};

inline bool readWholeFile(const std::filesystem::path &path, std::string *sink)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    sink->assign((std::istreambuf_iterator<char>(file)),
                 std::istreambuf_iterator<char>());
    return true;
}

inline bool string(const JsonValue &object,
                   const char *name,
                   std::string *sink)
{
    const auto *value = member(object, name);
    if (!value || value->kind != JsonValue::Kind::String) {
        return false;
    }
    *sink = value->stringValue;
    return true;
}

inline bool stringArray(const JsonValue &object,
                        const char *name,
                        std::vector<std::string> *sink)
{
    const auto *value = member(object, name);
    if (!value || value->kind != JsonValue::Kind::Array) {
        return false;
    }
    sink->clear();
    for (const auto &element : value->arrayValue) {
        if (element.kind != JsonValue::Kind::String) {
            return false;
        }
        sink->push_back(element.stringValue);
    }
    return true;
}

inline long long number(const JsonValue &object, const char *name, bool *found)
{
    const auto *value = member(object, name);
    if (found) {
        *found = value != nullptr;
    }
    if (!value || value->kind != JsonValue::Kind::Number) {
        return -1;
    }
    try {
        return std::stoll(value->stringValue);
    }
    catch (const std::exception &) {
        return -1;
    }
}

// Loads the staged package the way the host loader does and checks the parts of
// the Canonical v1 contract that apply to every Control package.
inline bool load(const std::filesystem::path &packageRoot,
                 Package *package,
                 const char *label)
{
    std::error_code code;
    const auto root = std::filesystem::weakly_canonical(packageRoot, code);
    if (code) {
        check(false, std::string(label) + ": package root resolves");
        return false;
    }
    package->packageRoot   = root;
    package->manifestPath  = std::filesystem::weakly_canonical(
        root / "plugin.json", code);
    check(!code && isWithin(root, package->manifestPath),
          std::string(label) + ": staged manifest stays inside the package root");
    if (code) {
        return false;
    }

    std::string manifest;
    if (!readWholeFile(package->manifestPath, &manifest)) {
        check(false, std::string(label) + ": staged plugin.json is readable");
        return false;
    }

    try {
        package->root = JsonParser(manifest).parse();
    }
    catch (const std::exception &error) {
        check(false, std::string(label) + ": staged plugin.json parses: "
                         + error.what());
        return false;
    }
    if (package->root.kind != JsonValue::Kind::Object) {
        check(false, std::string(label) + ": manifest root is an object");
        return false;
    }

    const auto *schema = member(package->root, "schema_version");
    check(schema && schema->kind == JsonValue::Kind::Number
              && schema->stringValue == "1",
          std::string(label) + ": schema_version is 1");
    check(string(package->root, "id", &package->id)
              && string(package->root, "name", &package->name)
              && string(package->root, "version", &package->version)
              && string(package->root, "appMinVersion", &package->appMinVersion)
              && string(package->root, "backend", &package->backend)
              && string(package->root, "command", &package->command),
          std::string(label) + ": required string fields are present");
    check(string(package->root, "description", &package->description),
          std::string(label) + ": description is present");
    check(stringArray(package->root, "capabilities", &package->capabilities),
          std::string(label) + ": capabilities is a string array");
    check(stringArray(package->root, "extensions", &package->extensions),
          std::string(label) + ": extensions is a string array");
    check(stringArray(package->root, "arguments", &package->arguments),
          std::string(label) + ": arguments is a string array");
    check(package->backend == "process",
          std::string(label) + ": backend is process");
    check(package->capabilities.size() == 1
              && package->capabilities.front() == "control",
          std::string(label) + ": capabilities contains only control");

    const auto *exitCodes = member(package->root, "success_exit_codes");
    if (exitCodes && exitCodes->kind == JsonValue::Kind::Array) {
        for (const auto &element : exitCodes->arrayValue) {
            if (element.kind == JsonValue::Kind::Number) {
                package->successExitCodes.push_back(std::stoll(
                    element.stringValue));
            }
        }
    }
    check(package->successExitCodes.size() == 1
              && package->successExitCodes.front() == 0,
          std::string(label) + ": success_exit_codes is [0]");

    package->timeoutMs  = number(package->root, "timeout_ms",
                                 &package->hasTimeout);
    check(package->hasTimeout && package->timeoutMs > 0,
          std::string(label) + ": timeout_ms is a positive integer");

    // Property-only fields and extra invocation names must not leak into a
    // Control-only package.
    check(member(package->root, "result_schema") == nullptr
              && member(package->root, "no_cache") == nullptr
              && member(package->root, "formats") == nullptr
              && member(package->root, "capability") == nullptr
              && member(package->root, "invocations") == nullptr
              && member(package->root, "interpreter") == nullptr,
          std::string(label)
              + ": manifest carries no Property-only or legacy fields");

    const std::filesystem::path commandName(package->command);
    check(!commandName.is_absolute() && !commandName.has_parent_path(),
          std::string(label) + ": command is package-relative");
    if (commandName.is_absolute() || commandName.has_parent_path()) {
        return false;
    }
    package->commandPath = std::filesystem::weakly_canonical(
        root / commandName, code);
    check(!code && std::filesystem::is_regular_file(package->commandPath),
          std::string(label) + ": staged command exists as a regular file");
    check(isWithin(root, package->commandPath),
          std::string(label) + ": staged command stays inside the package root");
    return true;
}

}  // namespace ManifestAssert
