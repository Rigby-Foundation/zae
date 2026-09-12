/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Userspace self test: libc basics, fork/exec/wait, filesystem. Exit code = failures. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mount.h>

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s (errno %d)\n", msg, errno); fails++; } } while (0)

static __thread int tls_var = 5;

int main(int argc, char **argv)
{
    printf("[test] pid %d starting, argv[0]=%s\n", getpid(), argv[0]);
    CHECK(argc == 1, "argc");
    CHECK(tls_var == 5, "TLS initialised");
    tls_var++;
    CHECK(tls_var == 6, "TLS writable");
    CHECK(getenv("PATH") && strcmp(getenv("PATH"), "/bin") == 0, "environment passed");

    /* heap: small and large (brk and mmap paths) */
    char *p = malloc(100);
    char *big = malloc(1 << 20);
    CHECK(p && big, "malloc");
    memset(big, 0x5A, 1 << 20);
    CHECK(big[(1 << 20) - 1] == 0x5A, "large allocation usable");
    free(p);
    free(big);

    /* stdio + floating point */
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f %d %s", 3.14159, 42, "ok");
    CHECK(strcmp(buf, "3.14 42 ok") == 0, "snprintf with floats");

    /* fork: child sees 0, parent sees pid; memory is copied, not shared */
    volatile int shared = 1;
    pid_t pid = fork();
    CHECK(pid >= 0, "fork failed");
    if (pid == 0) {
        shared = 2;
        _exit(7);
    }
    int status = -1;
    CHECK(waitpid(pid, &status, 0) == pid, "waitpid returned wrong pid");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 7, "child exit status");
    CHECK(shared == 1, "fork shared memory with the child");

    /* exec via PATH, with argv */
    pid = fork();
    if (pid == 0) {
        execlp("echo", "echo", "[test]", "exec", "works", (char *)NULL);
        _exit(99);
    }
    waitpid(pid, &status, 0);
    CHECK(WEXITSTATUS(status) == 0, "exec'd echo failed");

    /* exec of a missing file fails and returns */
    pid = fork();
    if (pid == 0) {
        execl("/bin/does-not-exist", "nope", (char *)NULL);
        _exit(errno == ENOENT ? 5 : 6);
    }
    waitpid(pid, &status, 0);
    CHECK(WEXITSTATUS(status) == 5, "exec of missing file did not fail with ENOENT");

    /* an ELF without sic's OS/ABI byte is refused before it can run */
    {
        FILE *in = fopen("/bin/echo", "r"), *out = fopen("/tmp/foreign", "w");
        char chunk[4096];
        size_t n;
        int first = 1;
        while (in && out && (n = fread(chunk, 1, sizeof(chunk), in)) > 0) {
            if (first) { chunk[7] = 0; first = 0; }       /* ELFOSABI_NONE, like a Linux binary */
            fwrite(chunk, 1, n, out);
        }
        if (in) fclose(in);
        if (out) fclose(out);
        pid = fork();
        if (pid == 0) {
            execl("/tmp/foreign", "foreign", (char *)NULL);
            _exit(errno == ENOEXEC ? 21 : 22);
        }
        waitpid(pid, &status, 0);
        CHECK(WEXITSTATUS(status) == 21, "foreign-ABI ELF was not rejected with ENOEXEC");
        unlink("/tmp/foreign");
    }

    /* a crashing child is reported as a signal, and we survive */
    pid = fork();
    if (pid == 0) {
        volatile int *bad = (int *)0x100000;
        *bad = 1;
        _exit(0);
    }
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == 11, "faulting child not reported as SIGSEGV");

    /* filesystem through stdio and POSIX calls */
    mkdir("/tmp/t", 0755);
    FILE *f = fopen("/tmp/t/a.txt", "w");
    CHECK(f != NULL, "fopen for writing");
    fprintf(f, "hello, %s\n", "tmpfs");
    fclose(f);

    f = fopen("/tmp/t/a.txt", "r");
    CHECK(f && fgets(buf, sizeof(buf), f) && strcmp(buf, "hello, tmpfs\n") == 0, "fgets read back");
    CHECK(fseek(f, 7, SEEK_SET) == 0 && fgets(buf, sizeof(buf), f) && strcmp(buf, "tmpfs\n") == 0, "fseek");
    fclose(f);

    struct stat st;
    int sr = stat("/tmp/t/a.txt", &st);
    if (sr != 0 || st.st_size != 13 || !S_ISREG(st.st_mode))
        printf("  stat: rc=%d size=%lld mode=%o\n", sr, (long long)st.st_size, st.st_mode);
    CHECK(sr == 0 && st.st_size == 13 && S_ISREG(st.st_mode), "stat");
    CHECK(stat("/bin/sh", &st) == 0 && st.st_size > 0, "initrd file present");
    CHECK(access("/nope", F_OK) != 0 && errno == ENOENT, "access ENOENT");

    int seen = 0;
    DIR *d = opendir("/bin");
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (strcmp(e->d_name, "sh") == 0) seen = 1;
    if (d) closedir(d);
    CHECK(seen, "readdir did not list /bin/sh");

    int fd = open("/tmp/t/a.txt", O_RDONLY);
    int fd2 = dup(fd);
    CHECK(fd >= 0 && fd2 > fd && read(fd2, buf, 5) == 5, "dup");
    close(fd); close(fd2);

    CHECK(unlink("/tmp/t/a.txt") == 0 && stat("/tmp/t/a.txt", &st) != 0, "unlink");
    CHECK(rmdir("/tmp/t") == 0, "rmdir");
    CHECK(chdir("/bin") == 0 && getcwd(buf, sizeof(buf)) && strcmp(buf, "/bin") == 0, "chdir/getcwd");

    /* zaefs on the NVMe disk: format if needed, mount, exercise, count boots */
    if (stat("/dev/nvme0n1", &st) == 0) {
        if (mount("/dev/nvme0n1", "/disk", "zaefs", 0, NULL) != 0 && errno != EBUSY) {
            pid = fork();
            if (pid == 0) { execl("/bin/mkfs.zaefs", "mkfs.zaefs", "-L", "sicdisk", "/dev/nvme0n1", (char *)NULL); _exit(127); }
            waitpid(pid, &status, 0);
            CHECK(WEXITSTATUS(status) == 0, "mkfs.zaefs");
            CHECK(mount("/dev/nvme0n1", "/disk", "zaefs", 0, NULL) == 0, "mount freshly formatted zaefs");
        }
        int boots = 0;
        f = fopen("/disk/boot_count", "r");
        if (f) { fscanf(f, "%d", &boots); fclose(f); }
        boots++;
        f = fopen("/disk/boot_count", "w");
        CHECK(f != NULL, "create file on zaefs");
        if (f) { fprintf(f, "%d\n", boots); fclose(f); }
        printf("[test] zaefs: this is boot #%d of this disk\n", boots);

        mkdir("/disk/dir", 0755);
        f = fopen("/disk/dir/big.bin", "w");
        CHECK(f != NULL, "create in zaefs subdir");
        if (f) {
            char blk[4096];
            for (int i = 0; i < 100; i++) { memset(blk, i, sizeof(blk)); fwrite(blk, 1, sizeof(blk), f); }  /* 400 KiB: uses the indirect block */
            fclose(f);
        }
        f = fopen("/disk/dir/big.bin", "r");
        int ok = f != NULL;
        if (f) {
            char blk[4096];
            CHECK(fseek(f, 77 * 4096 + 100, SEEK_SET) == 0 && fread(blk, 1, 50, f) == 50, "zaefs read at offset");
            for (int i = 0; i < 50; i++) ok &= blk[i] == 77;
            fclose(f);
        }
        CHECK(ok, "zaefs data integrity across the indirect block");
        CHECK(stat("/disk/dir/big.bin", &st) == 0 && st.st_size == 409600, "zaefs stat size");
        seen = 0;
        d = opendir("/disk/dir");
        while (d && (e = readdir(d))) if (strcmp(e->d_name, "big.bin") == 0) seen = 1;
        if (d) closedir(d);
        CHECK(seen, "zaefs readdir");
        CHECK(unlink("/disk/dir/big.bin") == 0 && rmdir("/disk/dir") == 0, "zaefs unlink/rmdir");
        CHECK(stat("/disk/dir", &st) != 0, "zaefs dir gone after rmdir");
    } else {
        printf("[test] no /dev/nvme0n1, skipping zaefs\n");
    }

    /* loadable kernel modules: insmod creates /dev/hello, rmmod removes it */
    pid = fork();
    if (pid == 0) { execlp("insmod", "insmod", "/lib/modules/hello.ko", (char *)NULL); _exit(127); }
    waitpid(pid, &status, 0);
    CHECK(WEXITSTATUS(status) == 0, "insmod hello.ko");
    f = fopen("/dev/hello", "r");
    CHECK(f && fgets(buf, sizeof(buf), f) && strncmp(buf, "hello from a kernel module", 26) == 0, "read /dev/hello");
    if (f) fclose(f);
    pid = fork();
    if (pid == 0) { execlp("insmod", "insmod", "/lib/modules/hello.ko", (char *)NULL); _exit(127); }
    waitpid(pid, &status, 0);
    CHECK(WEXITSTATUS(status) != 0, "loading the same module twice must fail");
    pid = fork();
    if (pid == 0) { execlp("rmmod", "rmmod", "hello", (char *)NULL); _exit(127); }
    waitpid(pid, &status, 0);
    CHECK(WEXITSTATUS(status) == 0, "rmmod hello");
    CHECK(stat("/dev/hello", &st) != 0, "/dev/hello gone after rmmod");

    /* the C compiler on the OS: compile a program with tcc, then run it */
    f = fopen("/tmp/prog.c", "w");
    fputs("#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
          "int main(int argc, char **argv) {\n"
          "  char *s = malloc(32); strcpy(s, \"compiled on sic\");\n"
          "  printf(\"[prog] %s: argc=%d, 2+2=%d, pi=%.3f\\n\", s, argc, 2 + 2, 3.14159);\n"
          "  return argc == 2 && strcmp(argv[1], \"x\") == 0 ? 33 : 1;\n}\n", f);
    fclose(f);
    pid = fork();
    if (pid == 0) {
        execlp("tcc", "tcc", "/tmp/prog.c", "-o", "/tmp/prog", (char *)NULL);
        _exit(127);
    }
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "tcc failed to compile /tmp/prog.c");
    pid = fork();
    if (pid == 0) {
        execl("/tmp/prog", "prog", "x", (char *)NULL);
        _exit(126);
    }
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 33, "tcc-compiled program did not run correctly");

    /* mmap: executable anonymous memory (what tcc -run needs) and a file mapping */
    {
        #include <sys/mman.h>
        unsigned char *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(code != MAP_FAILED, "mmap PROT_EXEC");
        if (code != MAP_FAILED) {
            /* mov eax, 42 ; ret */
            unsigned char insn[] = { 0xb8, 0x2a, 0, 0, 0, 0xc3 };
            memcpy(code, insn, sizeof(insn));
            int (*fn)(void) = (int (*)(void))code;
            CHECK(fn() == 42, "executing mmap'd code");
            CHECK(mprotect(code, 4096, PROT_READ) == 0, "mprotect");
            pid = fork();
            if (pid == 0) { code[0] = 1; _exit(0); }        /* must fault now */
            waitpid(pid, &status, 0);
            CHECK(WIFSIGNALED(status), "write to PROT_READ page was not a fault");
            munmap(code, 4096);
        }
        int mfd = open("/etc/motd", O_RDONLY);
        char *m = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, mfd, 0);
        CHECK(m != MAP_FAILED && strncmp(m, "Welcome", 7) == 0, "file mmap");
        if (m != MAP_FAILED) munmap(m, 4096);
        close(mfd);
    }

    /* tcc -run: compile straight to memory and execute */
    f = fopen("/tmp/run.c", "w");
    fputs("#include <stdio.h>\nint main(void){ printf(\"[run] tcc -run works\\n\"); return 44; }\n", f);
    fclose(f);
    pid = fork();
    if (pid == 0) {
        execlp("tcc", "tcc", "-run", "/tmp/run.c", (char *)NULL);
        _exit(127);
    }
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 44, "tcc -run");

    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    usleep(20000);
    clock_gettime(CLOCK_MONOTONIC, &b);
    long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    CHECK(ms >= 20 && ms < 60, "usleep/clock_gettime");

    printf("[test] done: %d failure(s)\n", fails);
    return fails;
}
