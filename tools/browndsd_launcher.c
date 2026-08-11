#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    char exePath[PATH_MAX];
    uint32_t exePathSize = sizeof(exePath);
    if (_NSGetExecutablePath(exePath, &exePathSize) != 0) {
        fprintf(stderr, "Executable path is too long\n");
        return 1;
    }

    char* lastSlash = strrchr(exePath, '/');
    if (!lastSlash) {
        fprintf(stderr, "Could not locate executable directory\n");
        return 1;
    }
    *lastSlash = '\0';

    char realExe[PATH_MAX];
    snprintf(realExe, sizeof(realExe), "%s/sdrpp-bin", exePath);

    const char* home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "HOME is not set\n");
        return 1;
    }

    char root[PATH_MAX];
    snprintf(root, sizeof(root), "%s/Library/Application Support/sdrpp", home);

    if (chdir(exePath) != 0) {
        perror("chdir");
        return 1;
    }

    char** args = calloc((size_t)argc + 4, sizeof(char*));
    if (!args) {
        perror("calloc");
        return 1;
    }

    int out = 0;
    args[out++] = realExe;
    args[out++] = "-r";
    args[out++] = root;
    for (int i = 1; i < argc; i++) {
        args[out++] = argv[i];
    }
    args[out] = NULL;

    execv(realExe, args);
    perror("execv");
    return 1;
}
