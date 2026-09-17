#pragma once
#include <chrono>
#include <string>
#include <thread>
#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#else
extern char** environ;
#endif
#endif

// UI-thread owned. Never accessed by a decoder callback. Only signals/reaps
// the exact child it created; an existing dashboard on the port is untouched.
class DashboardService {
public:
    DashboardService() = default;
    DashboardService(const DashboardService&) = delete;
    DashboardService& operator=(const DashboardService&) = delete;
    ~DashboardService() {
        stop();
#ifndef _WIN32
        while (pid > 0) { poll(); if (pid > 0) std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
#endif
    }
    bool active() { poll(); return running; }
    const std::string& status() const { return message; }
    bool start(const std::string& python, const std::string& script,
               const std::string& source, int port, const std::string& log) {
        poll();
        if (running) return false;
        if (python.empty() || script.empty() || source.empty() || port < 1 || port > 65535) {
            message = "Set Python, server.py, JSONL path and a valid port before starting.";
            return false;
        }
#ifdef _WIN32
        message = "Module launcher currently supports macOS/Linux. Use Terminal on Windows.";
        return false;
#else
        int pipefd[2];
        if (pipe(pipefd) != 0) { message = strerror(errno); return false; }
        fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
        fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
        posix_spawn_file_actions_t actions;
        int error = posix_spawn_file_actions_init(&actions);
        if (error) { close(pipefd[0]); close(pipefd[1]); message = strerror(error); return false; }
        auto check = [&](int result) { if (!error) error = result; };
        check(posix_spawn_file_actions_adddup2(&actions, pipefd[0], STDIN_FILENO));
        check(posix_spawn_file_actions_addclose(&actions, pipefd[1]));
        check(posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600));
        check(posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO));
        std::string portText = std::to_string(port);
        char* argv[] = {const_cast<char*>(python.c_str()), const_cast<char*>(script.c_str()),
            const_cast<char*>("--source"), const_cast<char*>(source.c_str()),
            const_cast<char*>("--port"), const_cast<char*>(portText.c_str()),
            const_cast<char*>("--managed"), nullptr};
#ifdef __APPLE__
        char** environment = *_NSGetEnviron();
#else
        char** environment = environ;
#endif
        posix_spawnattr_t attributes;
        int attrError = posix_spawnattr_init(&attributes);
        check(attrError);
#ifdef __APPLE__
        // Do not keep unrelated SDR device/socket descriptors alive in Python.
        if (!attrError) check(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT));
#endif
        if (!error) error = posix_spawnp(&pid, python.c_str(), &actions, &attributes, argv, environment);
        if (!attrError) posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        close(pipefd[0]);
        if (error) { close(pipefd[1]); pid = -1; message = std::string("Launch failed: ") + strerror(error); return false; }
        input = pipefd[1]; running = true; stopping = false;
        message = "Process running; server startup/errors are in the log.";
        return true;
#endif
    }
    void stop() {
#ifndef _WIN32
        poll();
        if (pid > 0 && !stopping) {
            close(input); input = -1; // EOF asks Python to shut down and join its reader.
            stopping = true;
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            message = "Stopping dashboard...";
        }
#endif
    }
    void poll() {
#ifndef _WIN32
        if (pid <= 0) return;
        int result = 0;
        pid_t done = waitpid(pid, &result, WNOHANG);
        if (done == pid || (done < 0 && errno == ECHILD)) {
            if (input >= 0) close(input);
            input = -1; pid = -1; running = false;
            message = stopping ? "Stopped" : "Dashboard exited; check log for startup errors or port conflicts.";
        } else if (stopping && std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL); // child still owned and unreaped: no PID reuse
        }
#endif
    }
private:
    bool running = false;
    bool stopping = false;
    std::string message = "Stopped";
#ifndef _WIN32
    pid_t pid = -1;
    int input = -1;
    std::chrono::steady_clock::time_point deadline;
#endif
};
