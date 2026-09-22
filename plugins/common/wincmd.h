#pragma once

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "wintext.h"

// Unicode argument handling shared by the native Control helpers.
//
// ShellUiWorker::run() keeps its fixed three-argument worker layout for the
// controls that shipped before it. Helpers with arbitrary option lists use the
// shared launcher and the scanner below instead, so the old controls keep
// their existing argument contract byte for byte.
namespace WinCmd {

inline std::vector<std::wstring> commandLineArguments()
{
    int count = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!argv) {
        return {};
    }
    std::vector<std::wstring> arguments;
    arguments.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        arguments.emplace_back(argv[index]);
    }
    LocalFree(argv);
    return arguments;
}

// Serializes one argument so the receiving process's CRT argv parser
// (CommandLineToArgvW) reconstructs the original string. This is the boundary
// used by CreateProcessW and by ShellExecuteExW lpParameters; it is not the
// boundary used by cmd.exe, PowerShell, or Windows Terminal, which each have
// their own parsing rules.
inline std::wstring quoteArgument(const std::wstring &value)
{
    std::wstring result = L"\"";
    size_t backslashes   = 0;
    for (const auto character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
        }
        else {
            result.append(backslashes, L'\\');
        }
        result += character;
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    return result + L'"';
}

inline std::wstring joinArguments(const std::vector<std::wstring> &arguments)
{
    std::wstring result;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (index != 0) {
            result += L' ';
        }
        result += quoteArgument(arguments[index]);
    }
    return result;
}

// Option tables and lookups accept a name with or without its leading dashes;
// both spellings normalize to the bare lower-cased name.
inline std::wstring bareOptionName(const std::wstring &value)
{
    size_t begin = 0;
    while (begin < value.size() && (value[begin] == L'-' || value[begin] == L'/')) {
        ++begin;
    }
    return WinText::toLower(value.substr(begin));
}

struct OptionScan {
    bool ok = false;
    std::wstring error;
    std::vector<std::pair<std::wstring, std::wstring>> options;
    std::vector<std::wstring> positionals;
    std::vector<std::wstring> duplicates;
};

// Scans arguments[firstIndex..] against a table of value-less flags and
// value-taking options. Option names are compared without the leading dashes and
// case-insensitively, matching how the host presents them in Seer's Arguments
// editor.
inline OptionScan scanOptions(const std::vector<std::wstring> &arguments,
                              const size_t firstIndex,
                              const std::vector<std::wstring> &flags,
                              const std::vector<std::wstring> &values)
{
    OptionScan scan;
    for (size_t index = firstIndex; index < arguments.size(); ++index) {
        const std::wstring &raw = arguments[index];
        if (raw.size() < 2 || raw[0] != L'-') {
            scan.positionals.push_back(raw);
            continue;
        }
        size_t dashes = 0;
        while (dashes < raw.size() && raw[dashes] == L'-') {
            ++dashes;
        }
        if (dashes == 1 && raw[1] != L'-') {
            scan.error = L"short options are not supported: " + raw;
            return scan;
        }
        if (dashes == 0 || dashes == raw.size()) {
            scan.error = L"malformed option: " + raw;
            return scan;
        }

        const std::wstring name = raw.substr(dashes);
        const auto flagIt       = std::find_if(
            flags.begin(), flags.end(), [&](const std::wstring &candidate) {
                return bareOptionName(candidate) == bareOptionName(name);
            });
        const auto valueIt = std::find_if(
            values.begin(), values.end(), [&](const std::wstring &candidate) {
                return bareOptionName(candidate) == bareOptionName(name);
            });

        if (flagIt != flags.end()) {
            scan.options.emplace_back(bareOptionName(name), std::wstring());
            continue;
        }
        if (valueIt == values.end()) {
            scan.error = L"unknown option: " + raw;
            return scan;
        }
        if (index + 1 >= arguments.size()) {
            scan.error = L"option requires a value: " + raw;
            return scan;
        }
        scan.options.emplace_back(bareOptionName(name), arguments[++index]);
    }

    for (size_t left = 0; left < scan.options.size(); ++left) {
        for (size_t right = left + 1; right < scan.options.size(); ++right) {
            if (scan.options[left].first == scan.options[right].first) {
                scan.duplicates.push_back(scan.options[left].first);
            }
        }
    }
    scan.ok = true;
    return scan;
}

inline bool hasOption(const OptionScan &scan, const std::wstring &name)
{
    const auto wanted = bareOptionName(name);
    for (const auto &option : scan.options) {
        if (option.first == wanted) {
            return true;
        }
    }
    return false;
}

inline bool optionValue(const OptionScan &scan,
                        const std::wstring &name,
                        std::wstring *value)
{
    const auto wanted = bareOptionName(name);
    for (const auto &option : scan.options) {
        if (option.first == wanted) {
            if (value) {
                *value = option.second;
            }
            return true;
        }
    }
    return false;
}

// For options a caller accepts more than once, in the order they appeared.
inline std::vector<std::wstring> optionValues(const OptionScan &scan,
                                              const std::wstring &name)
{
    std::vector<std::wstring> values;
    const auto wanted = bareOptionName(name);
    for (const auto &option : scan.options) {
        if (option.first == wanted) {
            values.push_back(option.second);
        }
    }
    return values;
}

// Rejects an option that was supplied more than once so a mistyped Seer argument
// list cannot silently keep only the last value. Names listed in `repeatable`
// are exempt.
inline bool rejectDuplicates(const OptionScan &scan,
                             std::wstring *error,
                             const std::vector<std::wstring> &repeatable = {})
{
    for (const auto &name : scan.duplicates) {
        bool allowed = false;
        for (const auto &candidate : repeatable) {
            if (bareOptionName(candidate) == name) {
                allowed = true;
                break;
            }
        }
        if (allowed) {
            continue;
        }
        if (error) {
            *error = L"option specified more than once: --" + name;
        }
        return false;
    }
    return true;
}

}  // namespace WinCmd
