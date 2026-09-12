/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static int list(const char *path, int header)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        perror(path);
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        printf("%-20s %8lld\n", path, (long long)st.st_size);
        return 0;
    }
    if (header)
        printf("%s:\n", path);
    DIR *d = opendir(path);
    if (!d) {
        perror(path);
        return 1;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        if (stat(full, &st) != 0)
            continue;
        const char *tag = S_ISDIR(st.st_mode) ? "/" : S_ISCHR(st.st_mode) ? "@" : "";
        printf("%s%-19s %8lld\n", e->d_name, tag, (long long)st.st_size);
    }
    closedir(d);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return list(".", 0);
    int rc = 0;
    for (int i = 1; i < argc; i++)
        rc |= list(argv[i], argc > 2);
    return rc;
}
