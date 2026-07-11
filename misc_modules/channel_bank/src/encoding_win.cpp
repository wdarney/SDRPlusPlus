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

static std::string winErr(DWORD err = GetLastError()) {
    return std::to_string((unsigned long)err);
}

static bool fileExists(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string joinPath(const std::string& base, const std::string& child) {
    if (base.empty()) return child;
    char last = base[base.size() - 1];
    if (last == '\\' || last == '/') return base + child;
    return base + "\\" + child;
}

static std::string firstMatchingDir(const std::string& parent, const std::string& pattern) {
    WIN32_FIND_DATAA data{};
    std::string query = joinPath(parent, pattern);
    HANDLE h = FindFirstFileA(query.c_str(), &data);
    if (h == INVALID_HANDLE_VALUE) return {};

    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            strcmp(data.cFileName, ".") != 0 &&
            strcmp(data.cFileName, "..") != 0) {
            std::string result = joinPath(parent, data.cFileName);
            FindClose(h);
            return result;
        }
    } while (FindNextFileA(h, &data));

    FindClose(h);
    return {};
}

static std::string envVar(const char* name) {
    char buf[MAX_PATH];
    DWORD len = GetEnvironmentVariableA(name, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    return std::string(buf, len);
}

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
        if (fileExists(g_ffmpegPath)) return g_ffmpegPath;
    }
    char buf[MAX_PATH];
    DWORD len = SearchPathA(NULL, "ffmpeg.exe", NULL, MAX_PATH, buf, NULL);
    if (len > 0 && len < MAX_PATH) return std::string(buf);

    std::string localAppData = envVar("LOCALAPPDATA");
    if (!localAppData.empty()) {
        std::string packages = joinPath(localAppData, "Microsoft\\WinGet\\Packages");
        std::string gyanPkg = firstMatchingDir(packages, "Gyan.FFmpeg_Microsoft.Winget.Source_*");
        if (!gyanPkg.empty()) {
            std::string ffmpegDir = firstMatchingDir(gyanPkg, "ffmpeg-*");
            if (!ffmpegDir.empty()) {
                std::string candidate = joinPath(ffmpegDir, "bin\\ffmpeg.exe");
                if (fileExists(candidate)) return candidate;
            }
        }
    }

    const char* programDirs[] = { "ProgramFiles", "ProgramFiles(x86)" };
    for (const char* envName : programDirs) {
        std::string root = envVar(envName);
        if (root.empty()) continue;
        std::string candidate = joinPath(root, "ffmpeg\\bin\\ffmpeg.exe");
        if (fileExists(candidate)) return candidate;
    }

    return {};
}

static bool runProcess(const std::string& cmdLine, DWORD timeoutMs = 600000) {
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

    if (wait != WAIT_OBJECT_0) {
        flog::error("[CBEncoding] ffmpeg timed out after {0}ms", std::to_string(timeoutMs));
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0) {
        flog::error("[CBEncoding] ffmpeg exited with code {0}", std::to_string(exitCode));
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

static void appendMetadataArgs(std::string& cmd, const std::string& transcript, float avgSnrDb) {
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
}

static std::string tempBasePath() {
    char tempDir[MAX_PATH];
    DWORD len = GetTempPathA(MAX_PATH, tempDir);
    if (len == 0 || len >= MAX_PATH) return {};

    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();
    DWORD tick = GetTickCount();
    return std::string(tempDir) + "channel_bank_" +
           std::to_string((unsigned long)pid) + "_" +
           std::to_string((unsigned long)tid) + "_" +
           std::to_string((unsigned long)tick);
}

std::string wavToM4A(const std::string& wavPath, const std::string& transcript, float avgSnrDb) {
    if (wavPath.size() < 4 || wavPath.substr(wavPath.size() - 4) != ".wav") return {};

    std::string ffmpeg = findFfmpeg();
    if (ffmpeg.empty()) {
        flog::error("[CBEncoding] ffmpeg not found — cannot encode M4A");
        return {};
    }

    std::string m4aPath = wavPath.substr(0, wavPath.size() - 4) + ".m4a";
    std::string tempBase = tempBasePath();
    if (tempBase.empty()) {
        flog::error("[CBEncoding] could not resolve Windows temp directory");
        return {};
    }
    std::string tempWav = tempBase + ".wav";
    std::string tempM4A = tempBase + ".m4a";

    // Network shares and mapped drives can make ffmpeg's direct read/write
    // unreliable or very slow. Copy to local temp first, encode locally, then
    // copy the finished M4A back to the recording folder.
    if (!CopyFileA(wavPath.c_str(), tempWav.c_str(), FALSE)) {
        flog::error("[CBEncoding] failed to stage WAV locally (err={0}): {1}",
                    winErr(), wavPath);
        DeleteFileA(tempWav.c_str());
        return {};
    }

    std::string cmd = escapeArg(ffmpeg)
        + " -y -hide_banner -loglevel error -i " + escapeArg(tempWav)
        + " -c:a aac -b:a 32k";

    bool hasTranscript = !transcript.empty();
    bool hasSnr        = (avgSnrDb != 0.0f);
    char snrBuf[32]    = {};
    if (hasSnr) snprintf(snrBuf, sizeof(snrBuf), "SNR: %.1f dB avg", avgSnrDb);

    appendMetadataArgs(cmd, transcript, avgSnrDb);

    cmd += " " + escapeArg(tempM4A);

    if (!runProcess(cmd)) {
        flog::error("[CBEncoding] ffmpeg encode failed: {0}", wavPath);
        DeleteFileA(tempWav.c_str());
        DeleteFileA(tempM4A.c_str());
        return {};
    }

    DeleteFileA(m4aPath.c_str());
    if (!CopyFileA(tempM4A.c_str(), m4aPath.c_str(), FALSE)) {
        flog::error("[CBEncoding] failed to copy M4A to destination (err={0}): {1}",
                    winErr(), m4aPath);
        DeleteFileA(tempWav.c_str());
        DeleteFileA(tempM4A.c_str());
        DeleteFileA(m4aPath.c_str());
        return {};
    }

    DeleteFileA(tempWav.c_str());
    DeleteFileA(tempM4A.c_str());

    setFileCaptureTime(m4aPath, captureTimeFromWavPath(wavPath));

    DeleteFileA(wavPath.c_str());
    flog::info("[CBEncoding] {0} (tags:{1}{2})", m4aPath,
               hasTranscript ? " transcript" : "", hasSnr ? snrBuf : "");
    return m4aPath;
}

} // namespace encoding
#endif // _WIN32
