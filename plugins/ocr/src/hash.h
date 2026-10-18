#pragma once

#include <map>
#include <string>
#include <vector>

namespace seer_ocr {

// Lowercase hex MD5 of an in-memory byte string; empty string on any crypto
// failure. Used only to fingerprint a path for output-file naming, never for
// integrity or security decisions.
std::string md5Hex(const std::string &data);

// Lowercase hex SHA-256 of a file; empty string on any read/crypto failure.
std::string sha256FileHex(const std::wstring &path);

// Parses `sha256sum`-style lines ("<hex> *<name>" or "<hex>  <name>"); blank
// lines and lines starting with '#' are ignored. Keys are bare file names.
bool loadSha256Sums(const std::wstring &sums_path,
                    std::map<std::string, std::string> &out);

// Verifies the given file names against <models_dir>\SHA256SUMS.
// A mismatch is a hard failure; a file without a pinned hash is reported in
// `log` and accepted, so an unpinned development drop can still run.
bool verifyModelFiles(const std::wstring &models_dir,
                      const std::vector<std::wstring> &file_names,
                      std::string &log);

}  // namespace seer_ocr
