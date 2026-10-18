#include "cleanup.h"

#include <windows.h>

namespace seer_ocr {

void cleanupPreviousRun(const OcrOutputPaths &paths)
{
    DeleteFileW(paths.txt_path.c_str());
    DeleteFileW(paths.json_path.c_str());
    DeleteFileW(paths.log_path.c_str());
    DeleteFileW(paths.txt_tmp_path.c_str());
    DeleteFileW(paths.json_tmp_path.c_str());
}

void sweepOldOutputs(const std::wstring &base_dir)
{
    if (base_dir.empty()) {
        return;
    }

    FILETIME now_ft;
    GetSystemTimeAsFileTime(&now_ft);
    ULARGE_INTEGER now_ul;
    now_ul.LowPart  = now_ft.dwLowDateTime;
    now_ul.HighPart = now_ft.dwHighDateTime;

    // 7 days in 100ns units: 7 * 24 * 3600 * 10,000,000
    const ULONGLONG kSevenDaysIn100ns = 7ULL * 86400ULL * 10000000ULL;
    if (now_ul.QuadPart <= kSevenDaysIn100ns) {
        return;
    }
    const ULONGLONG cutoff_time = now_ul.QuadPart - kSevenDaysIn100ns;

    std::wstring search_pattern = base_dir + L"\\*.ocr.*";
    WIN32_FIND_DATAW find_data;
    HANDLE hFind = FindFirstFileW(search_pattern.c_str(), &find_data);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            ULARGE_INTEGER file_ul;
            file_ul.LowPart  = find_data.ftLastWriteTime.dwLowDateTime;
            file_ul.HighPart = find_data.ftLastWriteTime.dwHighDateTime;

            if (file_ul.QuadPart < cutoff_time) {
                std::wstring file_path = base_dir + L"\\" + find_data.cFileName;
                DeleteFileW(file_path.c_str());
            }
        }
    } while (FindNextFileW(hFind, &find_data));

    FindClose(hFind);
}

}  // namespace seer_ocr
