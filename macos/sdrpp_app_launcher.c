#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    char launcher_path[PATH_MAX];
    uint32_t launcher_path_size = sizeof(launcher_path);
    if (_NSGetExecutablePath(launcher_path, &launcher_path_size) != 0) {
        fprintf(stderr, "Executable path is too long\n");
        return 1;
    }

    char* last_slash = strrchr(launcher_path, '/');
    if (!last_slash) {
        fprintf(stderr, "Could not locate executable directory\n");
        return 1;
    }
    *last_slash = '\0';

    char real_executable[PATH_MAX];
    int written = snprintf(real_executable, sizeof(real_executable), "%s/sdrpp-bin", launcher_path);
    if (written < 0 || (size_t)written >= sizeof(real_executable)) {
        fprintf(stderr, "SDR++ binary path is too long\n");
        return 1;
    }

    const char* user_home = getenv("HOME");
    if (!user_home) {
        fprintf(stderr, "HOME is not set\n");
        return 1;
    }

    char default_root[PATH_MAX];
    written = snprintf(default_root, sizeof(default_root), "%s/Library/Application Support/sdrpp", user_home);
    if (written < 0 || (size_t)written >= sizeof(default_root)) {
        fprintf(stderr, "SDR++ default root path is too long\n");
        return 1;
    }

    if (chdir(launcher_path) != 0) {
        perror("chdir");
        return 1;
    }

    char** forwarded_args = calloc((size_t)argc + 3, sizeof(char*));
    if (!forwarded_args) {
        perror("calloc");
        return 1;
    }

    int output_index = 0;
    forwarded_args[output_index++] = real_executable;
    forwarded_args[output_index++] = "--root";
    forwarded_args[output_index++] = default_root;
    for (int input_index = 1; input_index < argc; input_index++) {
        forwarded_args[output_index++] = argv[input_index];
    }
    forwarded_args[output_index] = NULL;

    execv(real_executable, forwarded_args);
    perror("execv");
    return 1;
}
