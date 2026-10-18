#include "hash.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

namespace seer_ocr {

namespace {

struct AlgHandle {
    BCRYPT_ALG_HANDLE handle = nullptr;
    ~AlgHandle() {
        if (handle) {
            BCryptCloseAlgorithmProvider(handle, 0);
        }
    }
};

struct HashHandle {
    BCRYPT_HASH_HANDLE handle = nullptr;
    ~HashHandle() {
        if (handle) {
            BCryptDestroyHash(handle);
        }
    }
};

std::string toHex(const unsigned char *data, size_t size)
{
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 0x0F]);
    }
    return out;
}

std::string toLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string toUtf8(const std::wstring &wstr)
{
    if (wstr.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::string trim(const std::string &value)
{
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

}  // namespace

std::string md5Hex(const std::string &data)
{
    AlgHandle alg;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg.handle, BCRYPT_MD5_ALGORITHM, nullptr, 0))) {
        return {};
    }

    DWORD object_size = 0;
    DWORD written = 0;
    if (!NT_SUCCESS(BCryptGetProperty(alg.handle, BCRYPT_OBJECT_LENGTH,
                                      reinterpret_cast<PUCHAR>(&object_size),
                                      sizeof(object_size), &written, 0))) {
        return {};
    }

    std::vector<unsigned char> object(object_size);
    HashHandle hash;
    if (!NT_SUCCESS(BCryptCreateHash(alg.handle, &hash.handle, object.data(), object_size,
                                     nullptr, 0, 0))) {
        return {};
    }

    if (!data.empty()
        && !NT_SUCCESS(BCryptHashData(hash.handle,
                                      reinterpret_cast<PUCHAR>(const_cast<char *>(data.data())),
                                      static_cast<ULONG>(data.size()), 0))) {
        return {};
    }

    unsigned char digest[16] = {0};
    if (!NT_SUCCESS(BCryptFinishHash(hash.handle, digest, sizeof(digest), 0))) {
        return {};
    }
    return toHex(digest, sizeof(digest));
}

std::string sha256FileHex(const std::wstring &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return {};
    }

    AlgHandle alg;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return {};
    }

    DWORD object_size = 0;
    DWORD written = 0;
    if (!NT_SUCCESS(BCryptGetProperty(alg.handle, BCRYPT_OBJECT_LENGTH,
                                      reinterpret_cast<PUCHAR>(&object_size),
                                      sizeof(object_size), &written, 0))) {
        return {};
    }

    std::vector<unsigned char> object(object_size);
    HashHandle hash;
    if (!NT_SUCCESS(BCryptCreateHash(alg.handle, &hash.handle, object.data(), object_size,
                                     nullptr, 0, 0))) {
        return {};
    }

    std::vector<char> buffer(64 * 1024);
    while (file.good()) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = file.gcount();
        if (got > 0
            && !NT_SUCCESS(BCryptHashData(hash.handle,
                                          reinterpret_cast<PUCHAR>(buffer.data()),
                                          static_cast<ULONG>(got), 0))) {
            return {};
        }
    }

    unsigned char digest[32] = {0};
    if (!NT_SUCCESS(BCryptFinishHash(hash.handle, digest, sizeof(digest), 0))) {
        return {};
    }
    return toHex(digest, sizeof(digest));
}

bool loadSha256Sums(const std::wstring &sums_path,
                    std::map<std::string, std::string> &out)
{
    out.clear();
    std::ifstream file(sums_path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }

        std::istringstream stream(line);
        std::string digest;
        std::string name;
        if (!(stream >> digest >> name)) {
            continue;
        }
        // sha256sum marks text mode with a leading '*'.
        while (!name.empty() && (name.front() == '*' || name.front() == ' ')) {
            name.erase(name.begin());
        }
        if (digest.size() != 64 || name.empty()) {
            continue;
        }
        out[toLowerAscii(name)] = toLowerAscii(digest);
    }
    return true;
}

bool verifyModelFiles(const std::wstring &models_dir,
                      const std::vector<std::wstring> &file_names,
                      std::string &log)
{
    std::map<std::string, std::string> sums;
    const std::wstring sums_path = models_dir + L"\\SHA256SUMS";
    const bool have_sums = loadSha256Sums(sums_path, sums);
    if (!have_sums) {
        log += "SHA256SUMS not readable; model hashes can not be verified\n";
    }

    for (const auto &wide_name : file_names) {
        const std::string name = toLowerAscii(toUtf8(wide_name));
        const auto it = sums.find(name);
        if (it == sums.end()) {
            log += "Unpinned model (no SHA256SUMS entry): " + name + "\n";
            continue;
        }

        const std::string actual = sha256FileHex(models_dir + L"\\" + wide_name);
        if (actual.empty()) {
            log += "Model hash check failed (unreadable file): " + name + "\n";
            return false;
        }
        if (actual != it->second) {
            log += "Model hash mismatch: " + name + "\n  expected " + it->second
                 + "\n  actual   " + actual + "\n";
            return false;
        }
        log += "Model hash ok: " + name + "\n";
    }

    return true;
}

}  // namespace seer_ocr
