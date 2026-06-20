// Windows M4A encoding via ffmpeg subprocess.
// macOS uses encoding.mm (AudioToolbox + AVFoundation).
// This file is only compiled on Windows.
#ifdef _WIN32
#include "encoding.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <utils/flog.h>

namespace encoding {

static std::string g_ffmpegPath;

void setFfmpegPath(const std::string& path) { g_ffmpegPath = path; }
std::string getFfmpegPath() { return g_ffmpegPath; }

static time_t captureTimeFromWavPath(const std::string& wavPath) {
    std::string base = wavPath;
    size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    if (base.size() >= 4 && base.substr(base.size() - 4) == ".wav")
        base = base.substr(0, base.size() - 4);

    size_t p1 = base.rfind('_');
    if (p1 == std::string::npos || p1 == 0) return 0;
    size_t p2 = base.rfind('_', p1 - 1);
    if (p2 == std::string::npos) return 0;

    std::string timePart = base.substr(p2 + 1, p1 - (p2 + 1));
    std::string datePart = base.substr(p1 + 1);

    int hh, mn, ss, dd, mo, yyyy;
    if (sscanf(timePart.c_str(), "%d-%d-%d", &hh, &mn, &ss) != 3) return 0;
    if (sscanf(datePart.c_str(), "%d-%d-%d", &dd, &mo, &yyyy) != 3) return 0;
    if (yyyy < 1970 || mo < 1 || mo > 12 || dd < 1 || dd > 31 ||
        hh < 0 || hh > 23 || mn < 0 || mn > 59 || ss < 0 || ss > 61) return 0;

    struct tm tmv = {};
    tmv.tm_hour = hh; tmv.tm_min = mn; tmv.tm_sec = ss;
    tmv.tm_mday = dd; tmv.tm_mon = mo - 1; tmv.tm_year = yyyy - 1900;
    tmv.tm_isdst = -1;
    return mktime(&tmv);
}

static void setFileCaptureTime(const std::string& path, time_t t) {
    if (t == 0) return;
    HANDLE hFile = CreateFileA(path.c_str(), FILE_WRITE_ATTRIBUTES, 0, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;

    ULONGLONG ull = ((ULONGLONG)t + 11644473600ULL) * 10000000ULL;
    FILETIME ft;
    ft.dwLowDateTime  = (DWORD)(ull & 0xFFFFFFFF);
    ft.dwHighDateTime = (DWORD)(ull >> 32);
    SetFileTime(hFile, &ft, &ft, &ft);
    CloseHandle(hFile);
}

static std::string findFfmpeg() {
    if (!g_ffmpegPath.empty()) {
        DWORD attr = GetFileAttributesA(g_ffmpegPath.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES) return g_ffmpegPath;
    }
    char buf[MAX_PATH];
    DWORD len = SearchPathA(NULL, "ffmpeg.exe", NULL, MAX_PATH, buf, NULL);
    if (len > 0 && len < MAX_PATH) return std::string(buf);
    return {};
}

static bool runProcess(const std::string& cmdLine, DWORD timeoutMs = 60000) {
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    std::string cmd = cmdLine;
    if (!CreateProcessA(NULL, &cmd[0], NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        flog::error("[CBEncoding] CreateProcess failed (err={0}): {1}",
                    std::to_string(GetLastError()), cmdLine);
        return false;
    }
    DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD exitCode = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (wait != WAIT_OBJECT_0) {
        flog::error("[CBEncoding] ffmpeg timed out after {0}ms", std::to_string(timeoutMs));
        return false;
    }
    return exitCode == 0;
}

static std::string escapeArg(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += '"';
    return out;
}

std::string wavToM4A(const std::string& wavPath, const std::string& transcript, float avgSnrDb) {
    if (wavPath.size() < 4 || wavPath.substr(wavPath.size() - 4) != ".wav") return {};

    std::string ffmpeg = findFfmpeg();
    if (ffmpeg.empty()) {
        flog::error("[CBEncoding] ffmpeg not found — cannot encode M4A");
        return {};
    }

    std::string m4aPath = wavPath.substr(0, wavPath.size() - 4) + ".m4a";

    std::string cmd = escapeArg(ffmpeg)
        + " -y -i " + escapeArg(wavPath)
        + " -c:a aac -b:a 32k";

    bool hasTranscript = !transcript.empty();
    bool hasSnr        = (avgSnrDb != 0.0f);
    char snrBuf[32]    = {};
    if (hasSnr) snprintf(snrBuf, sizeof(snrBuf), "SNR: %.1f dB avg", avgSnrDb);

    if (hasTranscript) {
        std::string escaped;
        for (char c : transcript) {
            if (c == '"') escaped += "\\\"";
            else if (c == '\n') escaped += " ";
            else escaped += c;
        }
        cmd += " -metadata lyrics=\"" + escaped + "\"";
    }
    if (hasSnr) {
        cmd += " -metadata comment=\"";
        cmd += snrBuf;
        cmd += "\"";
    }

    cmd += " " + escapeArg(m4aPath);

    if (!runProcess(cmd)) {
        flog::error("[CBEncoding] ffmpeg encode failed: {0}", wavPath);
        DeleteFileA(m4aPath.c_str());
        return {};
    }

    setFileCaptureTime(m4aPath, captureTimeFromWavPath(wavPath));

    DeleteFileA(wavPath.c_str());
    flog::info("[CBEncoding] {0} (tags:{1}{2})", m4aPath,
               hasTranscript ? " transcript" : "", hasSnr ? snrBuf : "");
    return m4aPath;
}

} // namespace encoding
#endif // _WIN32
