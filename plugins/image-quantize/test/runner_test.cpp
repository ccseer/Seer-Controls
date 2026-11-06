#include <windows.h>
#include <aclapi.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "lodepng.h"
#include "seer_quantize.h"

namespace {

std::wstring g_helper;
std::wstring g_data;
std::wstring g_root;

struct RunResult {
    DWORD code = 1u;
    std::string out;
    std::string err;
};

std::wstring join(const std::wstring& left, const std::wstring& right) {
    return left + (left.empty() ? L"" : L"\\") + right;
}

std::wstring native(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\", 0u) == 0u || path.size() < 248u) {
        return path;
    }
    if (path.rfind(L"\\\\", 0u) == 0u) {
        return L"\\\\?\\UNC\\" + path.substr(2u);
    }
    return L"\\\\?\\" + path;
}

bool exists_long(const std::wstring& path) {
    return GetFileAttributesW(native(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring quote(const std::wstring& text) { return L"\"" + text + L"\""; }

bool read_all(HANDLE pipe, std::string& text) {
    char buffer[4096];
    DWORD read = 0u;
    while (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0u) {
        text.append(buffer, read);
    }
    return true;
}

RunResult run(const std::wstring& arguments) {
    RunResult result;
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE out_read = nullptr;
    HANDLE out_write = nullptr;
    HANDLE err_read = nullptr;
    HANDLE err_write = nullptr;
    if (!CreatePipe(&out_read, &out_write, &attributes, 0u)
        || !CreatePipe(&err_read, &err_write, &attributes, 0u)) {
        return result;
    }
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0u);
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0u);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = out_write;
    startup.hStdError = err_write;
    startup.hStdInput = nullptr;

    PROCESS_INFORMATION process{};
    std::wstring command = quote(g_helper) + L" " + arguments;
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr,
                                        TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                        &startup, &process);
    CloseHandle(out_write);
    CloseHandle(err_write);
    if (!started) {
        CloseHandle(out_read);
        CloseHandle(err_read);
        return result;
    }
    read_all(out_read, result.out);
    read_all(err_read, result.err);
    WaitForSingleObject(process.hProcess, 120000u);
    GetExitCodeProcess(process.hProcess, &result.code);
    CloseHandle(out_read);
    CloseHandle(err_read);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}

unsigned count_lines(const std::string& text) {
    unsigned lines = 0u;
    for (char ch : text) {
        if (ch == '\n') {
            ++lines;
        }
    }
    return lines;
}

std::wstring target_for(const std::wstring& input, unsigned colors) {
    const std::size_t slash = input.find_last_of(L'\\');
    const std::size_t dot = input.find_last_of(L'.');
    const std::wstring directory = slash == std::wstring::npos
                                       ? std::wstring()
                                       : input.substr(0u, slash);
    const std::wstring stem =
        input.substr(slash == std::wstring::npos ? 0u : slash + 1u,
                     dot == std::wstring::npos ? std::wstring::npos
                                               : dot - (slash + 1u));
    wchar_t suffix[32];
    std::swprintf(suffix, 32u, L"-%uc.png", colors);
    return join(directory, stem + suffix);
}

std::wstring copy_fixture(const wchar_t* name) {
    const std::wstring source = join(g_data, name);
    const std::wstring target = join(g_root, name);
    CopyFileW(source.c_str(), target.c_str(), FALSE);
    return target;
}


bool has_chunk(const std::vector<unsigned char>& png, const char* type) {
    if (png.size() < 12u) {
        return false;
    }
    std::size_t offset = 8u;
    while (offset + 12u <= png.size()) {
        const std::uint32_t length =
            (static_cast<std::uint32_t>(png[offset]) << 24u)
            | (static_cast<std::uint32_t>(png[offset + 1u]) << 16u)
            | (static_cast<std::uint32_t>(png[offset + 2u]) << 8u)
            | static_cast<std::uint32_t>(png[offset + 3u]);
        if (std::memcmp(&png[offset + 4u], type, 4u) == 0) {
            return true;
        }
        offset += 12u + length;
    }
    return false;
}

// Denies creating files in the directory for the current user, which is the
// only reliable way to make a directory read-only on NTFS.
bool deny_directory_writes(const std::wstring& directory) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    DWORD size = 0u;
    GetTokenInformation(token, TokenUser, nullptr, 0u, &size);
    std::vector<unsigned char> buffer(size);
    const bool token_ok =
        GetTokenInformation(token, TokenUser, buffer.data(), size, &size) != 0;
    CloseHandle(token);
    if (!token_ok) {
        return false;
    }
    auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
    access.grfAccessMode = DENY_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(user->User.Sid);
    PACL acl = nullptr;
    if (SetEntriesInAclW(1u, &access, nullptr, &acl) != ERROR_SUCCESS) {
        return false;
    }
    const DWORD status = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(directory.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
    return status == ERROR_SUCCESS;
}

void allow_directory_writes(const std::wstring& directory) {
    SetNamedSecurityInfoW(
        const_cast<LPWSTR>(directory.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr);
}

std::vector<unsigned char> read_bytes(const std::wstring& path) {
    std::vector<unsigned char> bytes;
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return bytes;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(handle, &size);
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0u;
    ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read,
             nullptr);
    CloseHandle(handle);
    return bytes;
}

bool write_bytes(const std::wstring& path, const std::vector<unsigned char>& bytes) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0u, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0u;
    const bool ok = WriteFile(handle, bytes.data(),
                              static_cast<DWORD>(bytes.size()), &written, nullptr) != 0
                    && written == bytes.size();
    CloseHandle(handle);
    return ok;
}

// Returns the data length of the first chunk of this type, or SIZE_MAX.
std::size_t chunk_payload_size(const std::vector<unsigned char>& png, const char* type) {
    std::size_t offset = 8u;
    while (offset + 12u <= png.size()) {
        const std::uint32_t length =
            (static_cast<std::uint32_t>(png[offset]) << 24u)
            | (static_cast<std::uint32_t>(png[offset + 1u]) << 16u)
            | (static_cast<std::uint32_t>(png[offset + 2u]) << 8u)
            | static_cast<std::uint32_t>(png[offset + 3u]);
        if (std::memcmp(&png[offset + 4u], type, 4u) == 0) {
            return length;
        }
        offset += 12u + length;
    }
    return static_cast<std::size_t>(-1);
}

bool write_placeholder(const std::wstring& path) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0u, nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(handle);
    return true;
}

// AC-20: decode the helper's PNG with lodepng and rebuild the expected RGBA
// from the engine's own palette and indices. Any difference in PLTE, tRNS or
// the index plane changes a pixel.
void check_ac20(const wchar_t* fixture) {
    const std::wstring input = copy_fixture(fixture);
    const RunResult done = run(L"--input " + quote(input));
    SQ_CHECK_EQ(done.code, 0u);
    const std::wstring output = target_for(input, 256u);

    const std::vector<unsigned char> png = read_bytes(output);
    std::vector<unsigned char> actual;
    unsigned width = 0u;
    unsigned height = 0u;
    SQ_CHECK_EQ(lodepng::decode(actual, width, height, png), 0u);

    const std::vector<unsigned char> source_png = read_bytes(input);
    std::vector<unsigned char> source;
    unsigned source_width = 0u;
    unsigned source_height = 0u;
    SQ_CHECK_EQ(lodepng::decode(source, source_width, source_height, source_png), 0u);

    sq_params params{};
    SQ_CHECK_EQ(sq_params_init(&params), SQ_OK);
    params.color_space = SQ_SPACE_OKLAB;
    params.dither_mode = SQ_DITHER_FLOYD_STEINBERG;
    params.dither_strength = 0.5f;

    sq_image_in image{};
    image.struct_size = sizeof(image);
    image.version = SQ_ABI_VERSION;
    image.pixels = source.data();
    image.width = source_width;
    image.height = source_height;
    image.stride = static_cast<std::size_t>(source_width) * 4u;

    std::vector<unsigned char> indices(
        static_cast<std::size_t>(source_width) * source_height);
    sq_result result{};
    result.struct_size = sizeof(result);
    result.version = SQ_ABI_VERSION;
    result.indices = indices.data();
    result.indices_capacity = indices.size();
    SQ_CHECK_EQ(sq_quantize(&image, &params, nullptr, nullptr, &result, nullptr),
                SQ_OK);
    SQ_CHECK_EQ(width, source_width);
    SQ_CHECK_EQ(height, source_height);

    std::vector<unsigned char> expected(
        static_cast<std::size_t>(source_width) * source_height * 4u);
    for (std::size_t i = 0u; i < indices.size(); ++i) {
        const std::uint8_t* entry = result.palette_rgba + indices[i] * 4u;
        expected[i * 4u + 0u] = entry[0];
        expected[i * 4u + 1u] = entry[1];
        expected[i * 4u + 2u] = entry[2];
        expected[i * 4u + 3u] = entry[3];
    }
    SQ_CHECK(expected == actual);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: runner_test <image_quantize.exe> <fixture-data-dir>\n");
        return 2;
    }
    const std::wstring helper(argv[1], argv[1] + std::strlen(argv[1]));
    const std::wstring data(argv[2], argv[2] + std::strlen(argv[2]));
    g_helper = helper;
    g_data = data;

    wchar_t temporary[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temporary);
    wchar_t folder[MAX_PATH]{};
    std::swprintf(folder, MAX_PATH, L"%sseer-iq-runner-%lu", temporary,
                  GetCurrentProcessId());
    g_root = folder;
    CreateDirectoryW(g_root.c_str(), nullptr);

    const std::wstring alpha = copy_fixture(L"alpha_gradient_256x1.png");
    const std::wstring photo = copy_fixture(L"synthetic_2color_16x16.png");

    // Success is silent and writes exactly one file.
    {
        const RunResult done = run(L"--input " + quote(photo));
        SQ_CHECK_EQ(done.code, 0u);
        SQ_CHECK_EQ(done.out.size(), 0u);
        SQ_CHECK_EQ(done.err.size(), 0u);
        SQ_CHECK(exists_long(target_for(photo, 256u)));
    }

    // Usage and input failures: one line each, no stdout.
    {
        const RunResult missing = run(L"");
        SQ_CHECK_EQ(missing.code, 2u);
        SQ_CHECK_EQ(count_lines(missing.err), 1u);
        SQ_CHECK_EQ(missing.out.size(), 0u);
    }
    {
        const RunResult unknown = run(L"--input " + quote(photo) + L" --bogus");
        SQ_CHECK_EQ(unknown.code, 2u);
        SQ_CHECK_EQ(count_lines(unknown.err), 1u);
    }
    {
        const RunResult colors = run(L"--input " + quote(photo) + L" --colors 300");
        SQ_CHECK_EQ(colors.code, 2u);
        SQ_CHECK_EQ(count_lines(colors.err), 1u);
    }
    // The Deflate tier accepts two spellings and rejects the rest.
    {
        const RunResult named = run(L"--input " + quote(photo) + L" --deflate bogus");
        SQ_CHECK_EQ(named.code, 2u);
        SQ_CHECK_EQ(count_lines(named.err), 1u);
        const RunResult ranged = run(L"--input " + quote(photo) + L" --optimize 3");
        SQ_CHECK_EQ(ranged.code, 2u);
        SQ_CHECK_EQ(count_lines(ranged.err), 1u);
    }

    // Every tier writes the same pixels, max is never larger than fast, and the
    // diagnostic log records the tier.
    {
        const std::wstring source = copy_fixture(L"synthetic_gray_ramp_1024x1.png");
        const std::wstring fast_path = join(g_root, L"tier-fast.png");
        const std::wstring max_path = join(g_root, L"tier-max.png");
        const std::wstring log_path = join(g_root, L"tier.log");
        DeleteFileW(log_path.c_str());
        const RunResult fast = run(L"--input " + quote(source) + L" --output "
                                   + quote(fast_path) + L" --overwrite --deflate fast --log "
                                   + quote(log_path));
        SQ_CHECK_EQ(fast.code, 0u);
        const RunResult deep = run(L"--input " + quote(source) + L" --output "
                                   + quote(max_path) + L" --overwrite --optimize 2 --log "
                                   + quote(log_path));
        SQ_CHECK_EQ(deep.code, 0u);
        const std::vector<unsigned char> fast_png = read_bytes(fast_path);
        const std::vector<unsigned char> max_png = read_bytes(max_path);
        SQ_CHECK(fast_png.size() > 33u);
        SQ_CHECK(max_png.size() > 33u);
        SQ_CHECK(max_png.size() <= fast_png.size());
        lodepng::State fast_state;
        lodepng::State max_state;
        std::vector<unsigned char> fast_pixels;
        std::vector<unsigned char> max_pixels;
        unsigned fast_width = 0u;
        unsigned fast_height = 0u;
        unsigned max_width = 0u;
        unsigned max_height = 0u;
        SQ_CHECK_EQ(lodepng::decode(fast_pixels, fast_width, fast_height,
                                    fast_state, fast_png), 0u);
        SQ_CHECK_EQ(lodepng::decode(max_pixels, max_width, max_height, max_state,
                                    max_png), 0u);
        SQ_CHECK(fast_width == max_width && fast_height == max_height);
        SQ_CHECK(fast_pixels == max_pixels);
        const std::vector<unsigned char> log_bytes = read_bytes(log_path);
        const std::string log_text(log_bytes.begin(), log_bytes.end());
        SQ_CHECK(log_text.find("\"deflate\":\"fast\"") != std::string::npos);
        SQ_CHECK(log_text.find("\"deflate\":\"max\"") != std::string::npos);
    }
    {
        const RunResult absent = run(L"--input " + quote(join(g_root, L"none.png")));
        SQ_CHECK_EQ(absent.code, 6u);
        SQ_CHECK_EQ(count_lines(absent.err), 1u);
    }
    {
        const RunResult same = run(L"--input " + quote(photo) + L" --output "
                                   + quote(photo));
        SQ_CHECK_EQ(same.code, 2u);
        SQ_CHECK_EQ(count_lines(same.err), 1u);
    }

    // Collision advances to -2 instead of failing.
    {
        const std::wstring first = target_for(photo, 64u);
        DeleteFileW(first.c_str());
        SQ_CHECK(write_placeholder(first));
        const RunResult done = run(L"--input " + quote(photo) + L" --colors 64");
        SQ_CHECK_EQ(done.code, 0u);
        const std::wstring second = join(g_root, L"synthetic_2color_16x16-64c-2.png");
        SQ_CHECK(GetFileAttributesW(second.c_str()) != INVALID_FILE_ATTRIBUTES);
    }

    // Auto-naming exhaustion reports kBusy after 1000 attempts.
    {
        const std::wstring crowded = copy_fixture(L"synthetic_checkerboard_8x8.png");
        const std::wstring directory = g_root;
        const std::wstring stem = L"synthetic_checkerboard_8x8";
        for (unsigned attempt = 0u; attempt < 1000u; ++attempt) {
            wchar_t name[64];
            if (attempt == 0u) {
                std::swprintf(name, 64u, L"-%uc.png", 256u);
            } else {
                std::swprintf(name, 64u, L"-%uc-%u.png", 256u, attempt + 1u);
            }
            SQ_CHECK(write_placeholder(join(directory, stem + name)));
        }
        const RunResult done = run(L"--input " + quote(crowded));
        SQ_CHECK_EQ(done.code, 8u);
        SQ_CHECK_EQ(count_lines(done.err), 1u);
    }

    // A translucent entry must not push the tRNS chunk to the end of the
    // palette: the palette order keeps every non-opaque entry in front.
    {
        const unsigned width = 64u;
        const unsigned height = 64u;
        std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4u);
        for (unsigned i = 0u; i < width * height; ++i) {
            const unsigned color = i % 201u;
            const bool translucent = color == 200u;
            pixels[static_cast<std::size_t>(i) * 4u + 0u] =
                translucent ? 255u : static_cast<unsigned char>((color * 7u) & 0xFFu);
            pixels[static_cast<std::size_t>(i) * 4u + 1u] =
                translucent ? 255u : static_cast<unsigned char>((color * 13u) & 0xFFu);
            pixels[static_cast<std::size_t>(i) * 4u + 2u] =
                translucent ? 255u : static_cast<unsigned char>((color * 29u) & 0xFFu);
            pixels[static_cast<std::size_t>(i) * 4u + 3u] = translucent ? 128u : 255u;
        }
        std::vector<unsigned char> source_png;
        SQ_CHECK_EQ(lodepng::encode(source_png, pixels, width, height, LCT_RGBA, 8), 0u);
        const std::wstring source = join(g_root, L"bright-translucent.png");
        SQ_CHECK(write_bytes(source, source_png));
        for (const wchar_t* tier : {L"fast", L"balanced", L"max"}) {
            const std::wstring output =
                join(g_root, std::wstring(L"bright-translucent-") + tier + L".png");
            const RunResult done =
                run(L"--input " + quote(source) + L" --output " + quote(output)
                    + L" --overwrite --deflate " + tier);
            SQ_CHECK_EQ(done.code, 0u);
            const std::vector<unsigned char> png = read_bytes(output);
            SQ_CHECK(png.size() > 33u);
            const std::size_t trns = chunk_payload_size(png, "tRNS");
            SQ_CHECK(trns <= 4u);
        }
    }

    // Viewer mode: the auto-name is resolved before the wait starts, and a
    // viewer that cannot launch only costs the loading UI, never the output.
    {
        const std::wstring source = copy_fixture(L"synthetic_2color_16x16.png");
        DeleteFileW(target_for(source, 256u).c_str());
        SQ_CHECK(write_placeholder(target_for(source, 256u)));
        const RunResult done =
            run(L"--input " + quote(source)
                + L" --seerviewer C:////definitely////missing////viewer.exe");
        SQ_CHECK_EQ(done.code, 0u);
        SQ_CHECK(GetFileAttributesW(join(g_root, L"synthetic_2color_16x16-256c-2.png").c_str())
                 != INVALID_FILE_ATTRIBUTES);
    }
    {
        const RunResult missing = run(L"--input " + quote(photo) + L" --seerviewer");
        SQ_CHECK_EQ(missing.code, 2u);
        SQ_CHECK_EQ(count_lines(missing.err), 1u);
        const RunResult wait = run(L"--input " + quote(photo) + L" --wait-timeout=100");
        SQ_CHECK_EQ(wait.code, 2u);
        SQ_CHECK_EQ(count_lines(wait.err), 1u);
    }

    // AC-27: a non-ASCII directory survives the whole path.
    {
        const std::wstring unicode_dir = join(g_root, L"\u4e2d\u6587-\u00e9\u00e8");
        CreateDirectoryW(unicode_dir.c_str(), nullptr);
        const std::wstring input = join(unicode_dir, L"synthetic_2color_16x16.png");
        SQ_CHECK(CopyFileW(photo.c_str(), input.c_str(), FALSE) != 0);
        const RunResult done = run(L"--input " + quote(input));
        SQ_CHECK_EQ(done.code, 0u);
        SQ_CHECK(exists_long(target_for(input, 256u)));
    }

    // Explicit output needs --overwrite before it may replace a file.
    {
        const std::wstring explicit_path = join(g_root, L"explicit.png");
        write_placeholder(explicit_path);
        const RunResult refused = run(L"--input " + quote(photo) + L" --output "
                                      + quote(explicit_path));
        SQ_CHECK_EQ(refused.code, 8u);
        SQ_CHECK_EQ(count_lines(refused.err), 1u);
        const RunResult allowed = run(L"--input " + quote(photo) + L" --output "
                                      + quote(explicit_path) + L" --overwrite");
        SQ_CHECK_EQ(allowed.code, 0u);
        SQ_CHECK(read_bytes(explicit_path).size() > 32u);
    }

    // AC-23: two concurrent helpers both succeed with distinct outputs.
    {
        const std::wstring concurrent = copy_fixture(L"synthetic_256color_16x16.png");
        DeleteFileW(target_for(concurrent, 32u).c_str());
        DeleteFileW(join(g_root, L"synthetic_256color_16x16-32c-2.png").c_str());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION one{};
        PROCESS_INFORMATION two{};
        std::wstring command_one = quote(g_helper) + L" --input " + quote(concurrent)
                                   + L" --colors 32";
        std::wstring command_two = command_one;
        CreateProcessW(nullptr, command_one.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup, &one);
        CreateProcessW(nullptr, command_two.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup, &two);
        WaitForSingleObject(one.hProcess, 120000u);
        WaitForSingleObject(two.hProcess, 120000u);
        DWORD code_one = 1u;
        DWORD code_two = 1u;
        GetExitCodeProcess(one.hProcess, &code_one);
        GetExitCodeProcess(two.hProcess, &code_two);
        CloseHandle(one.hThread);
        CloseHandle(one.hProcess);
        CloseHandle(two.hThread);
        CloseHandle(two.hProcess);
        SQ_CHECK_EQ(code_one, 0u);
        SQ_CHECK_EQ(code_two, 0u);
        SQ_CHECK(GetFileAttributesW(target_for(concurrent, 32u).c_str())
                 != INVALID_FILE_ATTRIBUTES);
        SQ_CHECK(GetFileAttributesW(
                     join(g_root, L"synthetic_256color_16x16-32c-2.png").c_str())
                 != INVALID_FILE_ATTRIBUTES);
    }

    // Determinism: the thread count never changes the bytes.
    {
        const std::wstring one = join(g_root, L"det-1.png");
        const std::wstring four = join(g_root, L"det-4.png");
        SQ_CHECK_EQ(run(L"--input " + quote(photo) + L" --output " + quote(one)
                        + L" --overwrite --threads 1").code, 0u);
        SQ_CHECK_EQ(run(L"--input " + quote(photo) + L" --output " + quote(four)
                        + L" --overwrite --threads 4").code, 0u);
        SQ_CHECK(read_bytes(one) == read_bytes(four));
    }

    // --log appends exactly one JSON line.
    {
        const std::wstring log = join(g_root, L"run.log");
        DeleteFileW(log.c_str());
        SQ_CHECK_EQ(run(L"--input " + quote(photo) + L" --output "
                        + quote(join(g_root, L"logged.png"))
                        + L" --overwrite --log " + quote(log)).code, 0u);
        const std::vector<unsigned char> log_bytes = read_bytes(log);
        const std::string text(log_bytes.begin(), log_bytes.end());
        SQ_CHECK_EQ(count_lines(text), 1u);
    }

    // AC-27: a path beyond MAX_PATH still succeeds.
    {
        std::wstring deep = g_root;
        for (unsigned i = 0u; i < 24u; ++i) {
            deep += L"\\directory-segment-abcdef";
        }
        const std::wstring created = deep;
        std::wstring partial = g_root;
        for (unsigned i = 0u; i < 24u; ++i) {
            partial += L"\\directory-segment-abcdef";
            CreateDirectoryW(native(partial).c_str(), nullptr);
        }
        const std::wstring long_input = join(created, L"synthetic_2color_16x16.png");
        SQ_CHECK(CopyFileW(photo.c_str(), native(long_input).c_str(), FALSE) != 0);
        const RunResult done = run(L"--input " + quote(long_input));
        SQ_CHECK_EQ(done.code, 0u);
        SQ_CHECK_EQ(done.err.size(), 0u);
    }

    // A failing long path must still produce exactly one line, never a
    // truncation notice.
    {
        std::wstring deep = g_root;
        for (unsigned i = 0u; i < 24u; ++i) {
            deep += L"\\directory-segment-abcdef";
        }
        const RunResult done = run(L"--input " + quote(join(deep, L"missing.png")));
        SQ_CHECK_EQ(done.code, 6u);
        SQ_CHECK_EQ(count_lines(done.err), 1u);
        SQ_CHECK(done.err.find("truncated") == std::string::npos);
    }


    // A gray palette must still be written as an indexed PNG.
    {
        const std::wstring gray = copy_fixture(L"synthetic_gray_ramp_1024x1.png");
        SQ_CHECK_EQ(run(L"--input " + quote(gray)).code, 0u);
        const std::vector<unsigned char> png = read_bytes(target_for(gray, 256u));
        SQ_CHECK(png.size() > 33u);
        if (png.size() > 33u) {
            SQ_CHECK_EQ(static_cast<unsigned>(png[25]), 3u);
        }
        SQ_CHECK(has_chunk(png, "PLTE"));
    }

    // An opaque source must not gain a translucent palette entry. An empty
    // cluster used to be emitted with alpha 1 and then selected by real pixels,
    // which showed up as white speckles on a light background.
    {
        const std::wstring source = g_data + L"\\..\\real\\large_01_4k.png";
        const std::wstring large = join(g_root, L"large_01_4k.png");
        SQ_CHECK(CopyFileW(source.c_str(), large.c_str(), FALSE) != 0);
        SQ_CHECK_EQ(run(L"--input " + quote(large) + L" --deflate fast").code, 0u);
        const std::vector<unsigned char> png = read_bytes(target_for(large, 256u));
        lodepng::State state;
        std::vector<unsigned char> decoded;
        unsigned width = 0u;
        unsigned height = 0u;
        SQ_CHECK_EQ(lodepng::decode(decoded, width, height, state, png), 0u);
        SQ_CHECK_EQ(state.info_png.color.colortype, LCT_PALETTE);
        SQ_CHECK(state.info_png.color.palettesize > 0u);
        for (std::size_t i = 0u; i < state.info_png.color.palettesize; ++i) {
            SQ_CHECK_EQ(static_cast<unsigned>(
                            state.info_png.color.palette[i * 4u + 3u]), 255u);
        }
    }

    // AC-27: the extended-length form is accepted.
    {
        const std::wstring plain = copy_fixture(L"synthetic_2color_16x16.png");
        wchar_t full[MAX_PATH * 2]{};
        SQ_CHECK(GetFullPathNameW(plain.c_str(), MAX_PATH * 2, full, nullptr) != 0u);
        const std::wstring extended = std::wstring(L"\\\\?\\") + full;
        const RunResult done = run(L"--input " + quote(extended));
        SQ_CHECK_EQ(done.code, 0u);
        SQ_CHECK_EQ(done.err.size(), 0u);
    }

    // AC-27: a real UNC path, when this machine exposes the admin share.
    {
        wchar_t windows[MAX_PATH]{};
        GetWindowsDirectoryW(windows, MAX_PATH);
        const std::wstring share =
            std::wstring(L"\\\\localhost\\") + windows[0] + L"$";
        if (GetFileAttributesW(share.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::printf("runner_test: UNC case skipped, no admin share\n");
        } else {
            const std::wstring plain = copy_fixture(L"synthetic_2color_16x16.png");
            wchar_t full[MAX_PATH * 2]{};
            GetFullPathNameW(plain.c_str(), MAX_PATH * 2, full, nullptr);
            const std::wstring unc = std::wstring(L"\\\\localhost\\")
                                   + full[0] + L"$" + (full + 2);
            const RunResult done = run(L"--input " + quote(unc));
            SQ_CHECK_EQ(done.code, 0u);
            SQ_CHECK_EQ(done.err.size(), 0u);
        }
    }

    // AC-27: a read-only source directory fails with one line and no output.
    {
        const std::wstring dir = join(g_root, L"readonly-dir");
        CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring input = join(dir, L"synthetic_2color_16x16.png");
        SQ_CHECK(CopyFileW(join(g_data, L"synthetic_2color_16x16.png").c_str(),
                           input.c_str(), FALSE) != 0);
        if (!deny_directory_writes(dir)) {
            std::printf("runner_test: read-only case skipped, ACL not set\n");
        } else {
            const RunResult done = run(L"--input " + quote(input));
            SQ_CHECK_EQ(done.code, 1u);
            SQ_CHECK_EQ(count_lines(done.err), 1u);
            SQ_CHECK_EQ(done.out.size(), 0u);
            allow_directory_writes(dir);
        }
        DeleteFileW(input.c_str());
        RemoveDirectoryW(dir.c_str());
    }

    // AC-20 on a transparent fixture and a colour fixture.
    check_ac20(L"alpha_gradient_256x1.png");
    check_ac20(L"synthetic_256color_16x16.png");

    // 16-bit grayscale reduction maps (v + 128) / 257 exactly without WIC distortion.
    {
        const std::uint16_t samples[] = {128u, 129u, 385u, 386u, 32767u, 32896u};
        const std::uint8_t expected[] = {0u, 1u, 1u, 2u, 127u, 128u};
        std::vector<unsigned char> raw16(sizeof(samples));
        for (std::size_t i = 0u; i < 6u; ++i) {
            raw16[i * 2u + 0u] = static_cast<unsigned char>((samples[i] >> 8u) & 0xFFu);
            raw16[i * 2u + 1u] = static_cast<unsigned char>(samples[i] & 0xFFu);
        }
        std::vector<unsigned char> png16;
        SQ_CHECK_EQ(lodepng::encode(png16, raw16.data(), 6u, 1u, LCT_GREY, 16u), 0u);
        const std::wstring gray16_path = join(g_root, L"gray16_boundary.png");
        const std::wstring out_path = join(g_root, L"gray16_boundary_out.png");
        SQ_CHECK(write_bytes(gray16_path, png16));

        const RunResult done = run(L"--input " + quote(gray16_path) + L" --output "
                                   + quote(out_path) + L" --overwrite --dither none");
        SQ_CHECK_EQ(done.code, 0u);
        SQ_CHECK_EQ(done.err.size(), 0u);

        const std::vector<unsigned char> out_png = read_bytes(out_path);
        std::vector<unsigned char> decoded;
        unsigned w = 0u;
        unsigned h = 0u;
        SQ_CHECK_EQ(lodepng::decode(decoded, w, h, out_png), 0u);
        SQ_CHECK_EQ(w, 6u);
        SQ_CHECK_EQ(h, 1u);
        for (std::size_t i = 0u; i < 6u; ++i) {
            SQ_CHECK_EQ(static_cast<unsigned>(decoded[i * 4u + 0u]), static_cast<unsigned>(expected[i]));
            SQ_CHECK_EQ(static_cast<unsigned>(decoded[i * 4u + 1u]), static_cast<unsigned>(expected[i]));
            SQ_CHECK_EQ(static_cast<unsigned>(decoded[i * 4u + 2u]), static_cast<unsigned>(expected[i]));
            SQ_CHECK_EQ(static_cast<unsigned>(decoded[i * 4u + 3u]), 255u);
        }
    }

    // ICO containing an RGB (color type 2) PNG frame is refused by WIC with
    // 0x88982F60 and exits with kUnsupported (exit code 5).
    {
        std::vector<unsigned char> rgb_pixels(16u * 16u * 3u, 0u);
        for (std::size_t i = 0u; i < 16u * 16u; ++i) {
            rgb_pixels[i * 3u + 0u] = 255u;  // Red
        }
        std::vector<unsigned char> rgb_png;
        SQ_CHECK_EQ(lodepng::encode(rgb_png, rgb_pixels.data(), 16u, 16u, LCT_RGB, 8u), 0u);

        // Build ICO container: 6-byte header + 16-byte dir entry + PNG payload
        std::vector<unsigned char> ico(22u + rgb_png.size());
        ico[0] = 0; ico[1] = 0;  // idReserved
        ico[2] = 1; ico[3] = 0;  // idType = 1 (ICO)
        ico[4] = 1; ico[5] = 0;  // idCount = 1
        ico[6] = 16;             // bWidth
        ico[7] = 16;             // bHeight
        ico[8] = 0;              // bColorCount
        ico[9] = 0;              // bReserved
        ico[10] = 1; ico[11] = 0; // wPlanes
        ico[12] = 24; ico[13] = 0; // wBitCount
        const std::uint32_t png_size = static_cast<std::uint32_t>(rgb_png.size());
        ico[14] = static_cast<unsigned char>(png_size & 0xFFu);
        ico[15] = static_cast<unsigned char>((png_size >> 8u) & 0xFFu);
        ico[16] = static_cast<unsigned char>((png_size >> 16u) & 0xFFu);
        ico[17] = static_cast<unsigned char>((png_size >> 24u) & 0xFFu);
        ico[18] = 22; ico[19] = 0; ico[20] = 0; ico[21] = 0;  // dwImageOffset = 22
        std::memcpy(ico.data() + 22u, rgb_png.data(), rgb_png.size());

        const std::wstring ico_path = join(g_root, L"rgb_frame.ico");
        SQ_CHECK(write_bytes(ico_path, ico));

        const RunResult done = run(L"--input " + quote(ico_path));
        SQ_CHECK_EQ(done.code, 5u);
        SQ_CHECK_EQ(count_lines(done.err), 1u);
        SQ_CHECK(done.err.find("cannot decode") != std::string::npos);
        // Note: WIC typically reports 0x88982F60 (WINCODEC_ERR_BADIMAGE) here.
        SQ_CHECK_EQ(done.out.size(), 0u);
    }

    // A 16-bit image (gray or RGB) carrying an embedded ICC profile is refused
    // with kUnsupported (code 5) and an explicit error.
    {
        const std::wstring srgb_profile = copy_fixture(L"profile_srgb_8x8.png");
        const std::vector<unsigned char> profile_png = read_bytes(srgb_profile);
        std::vector<unsigned char> iccp_chunk;
        std::size_t offset = 8u;
        while (offset + 12u <= profile_png.size()) {
            const std::uint32_t len =
                (static_cast<std::uint32_t>(profile_png[offset]) << 24u)
                | (static_cast<std::uint32_t>(profile_png[offset + 1u]) << 16u)
                | (static_cast<std::uint32_t>(profile_png[offset + 2u]) << 8u)
                | static_cast<std::uint32_t>(profile_png[offset + 3u]);
            if (std::memcmp(&profile_png[offset + 4u], "iCCP", 4u) == 0) {
                iccp_chunk.assign(profile_png.begin() + offset,
                                  profile_png.begin() + offset + 12u + len);
                break;
            }
            offset += 12u + len;
        }
        SQ_CHECK(!iccp_chunk.empty());

        // 16-bit Gray with ICC
        const std::uint16_t gray_samples[] = {100u, 200u};
        std::vector<unsigned char> raw_gray(sizeof(gray_samples));
        raw_gray[0] = static_cast<unsigned char>((gray_samples[0] >> 8u) & 0xFFu);
        raw_gray[1] = static_cast<unsigned char>(gray_samples[0] & 0xFFu);
        raw_gray[2] = static_cast<unsigned char>((gray_samples[1] >> 8u) & 0xFFu);
        raw_gray[3] = static_cast<unsigned char>(gray_samples[1] & 0xFFu);
        std::vector<unsigned char> gray16_png;
        SQ_CHECK_EQ(lodepng::encode(gray16_png, raw_gray.data(), 2u, 1u, LCT_GREY, 16u), 0u);
        SQ_CHECK(gray16_png.size() >= 33u);
        gray16_png.insert(gray16_png.begin() + 33u, iccp_chunk.begin(), iccp_chunk.end());
        const std::wstring gray16_icc_path = join(g_root, L"gray16_with_icc.png");
        SQ_CHECK(write_bytes(gray16_icc_path, gray16_png));

        const RunResult gray_done = run(L"--input " + quote(gray16_icc_path));
        SQ_CHECK_EQ(gray_done.code, 5u);
        SQ_CHECK_EQ(count_lines(gray_done.err), 1u);
        SQ_CHECK(gray_done.err.find("a 16-bit image with an embedded profile is not supported")
                 != std::string::npos);
        SQ_CHECK_EQ(gray_done.out.size(), 0u);

        // 16-bit RGB with ICC
        const std::uint16_t rgb_samples[] = {100u, 200u, 300u, 400u, 500u, 600u};
        std::vector<unsigned char> raw_rgb(sizeof(rgb_samples));
        for (std::size_t i = 0u; i < 6u; ++i) {
            raw_rgb[i * 2u + 0u] = static_cast<unsigned char>((rgb_samples[i] >> 8u) & 0xFFu);
            raw_rgb[i * 2u + 1u] = static_cast<unsigned char>(rgb_samples[i] & 0xFFu);
        }
        std::vector<unsigned char> rgb16_png;
        SQ_CHECK_EQ(lodepng::encode(rgb16_png, raw_rgb.data(), 2u, 1u, LCT_RGB, 16u), 0u);
        SQ_CHECK(rgb16_png.size() >= 33u);
        rgb16_png.insert(rgb16_png.begin() + 33u, iccp_chunk.begin(), iccp_chunk.end());
        const std::wstring rgb16_icc_path = join(g_root, L"rgb16_with_icc.png");
        SQ_CHECK(write_bytes(rgb16_icc_path, rgb16_png));

        const RunResult rgb_done = run(L"--input " + quote(rgb16_icc_path));
        SQ_CHECK_EQ(rgb_done.code, 5u);
        SQ_CHECK_EQ(count_lines(rgb_done.err), 1u);
        SQ_CHECK(rgb_done.err.find("a 16-bit image with an embedded profile is not supported")
                 != std::string::npos);
        SQ_CHECK_EQ(rgb_done.out.size(), 0u);
    }

    return sqtest::finish();
}
