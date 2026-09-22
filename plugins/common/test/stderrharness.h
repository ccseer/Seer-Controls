#pragma once

#include <windows.h>

#include <string>

// Captures the calling process's standard-error handle so a test can assert what
// a helper told the host, and so host-facing text a test provokes does not leak
// into ctest output as unexplained noise.
//
// It lives here rather than in one package's test because more than one package
// asserts the same contract: every Control helper has to explain a refusal on
// the stream the host renders.
namespace StderrHarness {

class Capture {
public:
    Capture()
    {
        SECURITY_ATTRIBUTES inherited{sizeof(inherited), nullptr, TRUE};
        if (!CreatePipe(&m_read, &m_write, &inherited, 0))
            return;
        m_previous = GetStdHandle(STD_ERROR_HANDLE);
        m_armed    = SetStdHandle(STD_ERROR_HANDLE, m_write) != FALSE;
    }

    Capture(const Capture &) = delete;
    Capture &operator=(const Capture &) = delete;

    ~Capture()
    {
        if (m_armed)
            SetStdHandle(STD_ERROR_HANDLE, m_previous);
        if (m_write)
            CloseHandle(m_write);
        if (m_read)
            CloseHandle(m_read);
    }

    // Everything written since construction, up to the read end being drained.
    std::string drain()
    {
        if (!m_armed)
            return std::string();
        // Closing the write end turns the reads below into ERROR_BROKEN_PIPE
        // instead of a blocking wait for more data that never comes.
        CloseHandle(m_write);
        m_write = nullptr;
        std::string text;
        char buffer[512]{};
        DWORD got = 0;
        while (ReadFile(m_read, buffer, sizeof(buffer), &got, nullptr) && got > 0)
            text.append(buffer, static_cast<size_t>(got));
        return text;
    }

private:
    HANDLE m_read     = nullptr;
    HANDLE m_write    = nullptr;
    HANDLE m_previous = nullptr;
    bool m_armed      = false;
};

}  // namespace StderrHarness
