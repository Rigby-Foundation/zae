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
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <linux/fb.h>

static int fails;
static volatile sig_atomic_t got;
static void handler(int sig) { got = sig; }
static void segv(int sig, siginfo_t *si, void *uc) { (void)uc; _exit(si->si_addr == (void *)0x100000 && sig == SIGSEGV ? 6 : 7); }
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

    printf("[test] heap\n");
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

    printf("[test] fork/exec\n");
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

    printf("[test] filesystem\n");
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

    printf("[test] pipes\n");
    /* pipes: parent -> child through a pipe, EOF on close, SIGPIPE on write with no reader */
    {
        int pfd[2];
        CHECK(pipe(pfd) == 0, "pipe()");
        pid = fork();
        if (pid == 0) {
            close(pfd[1]);
            char rb[64];
            long tot = 0, r;
            while ((r = read(pfd[0], rb + tot, sizeof(rb) - tot)) > 0) tot += r;
            _exit(tot == 11 && strncmp(rb, "through the", 11) == 0 ? 9 : 1);
        }
        close(pfd[0]);
        CHECK(write(pfd[1], "through the", 11) == 11, "pipe write");
        close(pfd[1]);
        waitpid(pid, &status, 0);
        CHECK(WEXITSTATUS(status) == 9, "child read the pipe and saw EOF");

        /* a pipeline through the shell: yes | grep must end when grep exits (SIGPIPE) */
        pid = fork();
        if (pid == 0) {
            int p2[2]; pipe(p2);
            pid_t a = fork();
            if (a == 0) { dup2(p2[1], 1); close(p2[0]); close(p2[1]); execlp("yes", "yes", "line", (char *)NULL); _exit(127); }
            pid_t b = fork();
            if (b == 0) {
                int sink = open("/tmp/pipe.out", O_WRONLY | O_CREAT | O_TRUNC, 0644);
                dup2(p2[0], 0); if (sink >= 0) dup2(sink, 1);
                close(p2[0]); close(p2[1]);
                execlp("cat", "cat", (char *)NULL); _exit(127);
            }
            close(p2[0]); close(p2[1]);
            usleep(30000);
            kill(b, SIGKILL);               /* reader dies -> writer gets SIGPIPE */
            int sa, sb;
            waitpid(b, &sb, 0);
            waitpid(a, &sa, 0);
            _exit(WIFSIGNALED(sa) && WTERMSIG(sa) == SIGPIPE ? 0 : 1);
        }
        waitpid(pid, &status, 0);
        CHECK(WEXITSTATUS(status) == 0, "writer got SIGPIPE when the reader died");
    }

    printf("[test] signals\n");
    /* signals: handler runs, sigreturn restores state, default action kills, EINTR */
    {
        struct sigaction sa = { .sa_handler = handler };
        sigaction(SIGUSR1, &sa, NULL);
        raise(SIGUSR1);
        CHECK(got == SIGUSR1, "SIGUSR1 handler ran");

        got = 0;
        pid = fork();
        if (pid == 0) { pause(); _exit(got == SIGUSR2 ? 4 : 5); }
        sigaction(SIGUSR2, &sa, NULL);          /* child inherited it? no: set before fork next time */
        usleep(10000);
        kill(pid, SIGUSR2);
        waitpid(pid, &status, 0);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGUSR2, "default action of SIGUSR2 terminated the child");

        pid = fork();
        if (pid == 0) { pause(); _exit(got == SIGUSR2 ? 4 : 5); }   /* now inherits the handler */
        usleep(10000);
        kill(pid, SIGUSR2);
        waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 4, "pause() returned after the handler");

        /* a busy-looping child is killable (delivery from the timer interrupt) */
        pid = fork();
        if (pid == 0) { for (;;) ; }
        usleep(20000);
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM, "SIGTERM killed a spinning child");

        /* alarm + interrupted sleep */
        got = 0;
        sigaction(SIGALRM, &sa, NULL);
        alarm(1);
        struct timespec a0, a1;
        clock_gettime(CLOCK_MONOTONIC, &a0);
        int left = sleep(5);
        clock_gettime(CLOCK_MONOTONIC, &a1);
        long slept = (a1.tv_sec - a0.tv_sec) * 1000 + (a1.tv_nsec - a0.tv_nsec) / 1000000;
        CHECK(got == SIGALRM && left > 0 && slept < 2000, "alarm interrupted sleep()");

        /* handler for a fault: SIGSEGV with si_addr, then exit from the handler */
        pid = fork();
        if (pid == 0) {
            struct sigaction ss = { .sa_sigaction = segv, .sa_flags = SA_SIGINFO };
            sigaction(SIGSEGV, &ss, NULL);
            *(volatile int *)0x100000 = 1;
            _exit(8);
        }
        waitpid(pid, &status, 0);
        CHECK(WEXITSTATUS(status) == 6, "SIGSEGV handler got si_addr");

        /* blocked signals stay pending until unblocked */
        sigset_t set, old;
        sigemptyset(&set); sigaddset(&set, SIGUSR1);
        sigprocmask(SIG_BLOCK, &set, &old);
        got = 0;
        raise(SIGUSR1);
        CHECK(got == 0, "blocked signal was delivered early");
        sigpending(&set);
        CHECK(sigismember(&set, SIGUSR1), "sigpending shows the blocked signal");
        sigprocmask(SIG_SETMASK, &old, NULL);
        CHECK(got == SIGUSR1, "signal delivered on unblock");
    }

    printf("[test] disks\n");
    /* Disk tests format whole drives: only in a VM (the kernel sets SIC_VM). */
    const char *vm = getenv("SIC_VM");
    int in_vm = vm && *vm;
    if (!in_vm)
        printf("[test] not in a VM: skipping the disk-formatting tests\n");

    /* zaefs on the NVMe disk: format if needed, mount, exercise, count boots */
    if (in_vm && stat("/dev/nvme0n1", &st) == 0) {
        umount("/disk");
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
    if (access("/lib/modules/hello.ko", R_OK) == 0) {
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
    } else {
        printf("[test] no /lib/modules/hello.ko (modules not configured), skipping\n");
    }

    /* FAT on the second NVMe disk: format, mount, write, long names readable, unmount, remount */
    if (in_vm && stat("/dev/nvme1n1", &st) == 0) {
        mkdir("/fat", 0755);
        if (mount("/dev/nvme1n1", "/fat", "fat", 0, NULL) != 0 && errno != EBUSY) {
            pid = fork();
            if (pid == 0) { execl("/bin/mkfs.fat", "mkfs.fat", "-n", "SICFAT", "/dev/nvme1n1", (char *)NULL); _exit(127); }
            waitpid(pid, &status, 0);
            CHECK(WEXITSTATUS(status) == 0, "mkfs.fat");
            CHECK(mount("/dev/nvme1n1", "/fat", "fat", 0, NULL) == 0, "mount freshly formatted FAT");
        }
        mkdir("/fat/dir", 0755);
        f = fopen("/fat/dir/hello.txt", "w");
        CHECK(f != NULL, "create on FAT");
        if (f) { fputs("written by sic on FAT\n", f); fclose(f); }
        f = fopen("/fat/dir/big.bin", "w");
        if (f) {
            char blk[4096];
            for (int i = 0; i < 40; i++) { memset(blk, i, sizeof(blk)); fwrite(blk, 1, sizeof(blk), f); }   /* 160 KiB, many clusters */
            fclose(f);
        }
        f = fopen("/fat/dir/big.bin", "r");
        int ok = f != NULL;
        if (f) {
            char blk[4096];
            CHECK(fseek(f, 33 * 4096 + 7, SEEK_SET) == 0 && fread(blk, 1, 20, f) == 20, "FAT read at offset");
            for (int i = 0; i < 20; i++) ok &= blk[i] == 33;
            fclose(f);
        }
        CHECK(ok, "FAT data integrity across clusters");
        CHECK(stat("/fat/dir/big.bin", &st) == 0 && st.st_size == 163840, "FAT stat size");
        CHECK(umount("/fat") == 0, "umount FAT");
        CHECK(mount("/dev/nvme1n1", "/fat", "fat", 0, NULL) == 0, "remount FAT");
        f = fopen("/fat/dir/hello.txt", "r");
        CHECK(f && fgets(buf, sizeof(buf), f) && strcmp(buf, "written by sic on FAT\n") == 0, "FAT contents survive a remount");
        if (f) fclose(f);
        CHECK(unlink("/fat/dir/big.bin") == 0 && unlink("/fat/dir/hello.txt") == 0 && rmdir("/fat/dir") == 0, "FAT unlink/rmdir");
        CHECK(stat("/fat/dir", &st) != 0, "FAT dir gone");
        /* leave a file behind for the host to check */
        f = fopen("/fat/from_sic.txt", "w");
        if (f) { fprintf(f, "boot ok\n"); fclose(f); }
    } else {
        printf("[test] no /dev/nvme1n1, skipping FAT\n");
    }

    printf("[test] threads\n");
    /* threads: run the pthreads smoke test as a separate program */
    pid = fork();
    if (pid == 0) { execlp("threads", "threads", (char *)NULL); _exit(127); }
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "pthreads test program");

    /* the C compiler on the OS: compile a program with tcc, then run it (x86 only so far) */
    int have_tcc = access("/bin/tcc", X_OK) == 0;
    if (have_tcc) {
    printf("[test] tcc\n");
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
    }

#ifdef __ALTIVEC__
    /* AltiVec: the vector unit is enabled lazily and its state must survive
     * a context switch, a fork, and a signal handler that uses it too. */
    printf("[test] altivec\n");
    {
        typedef int v4si __attribute__((vector_size(16)));
        volatile v4si a = { 1, 2, 3, 4 }, b = { 10, 20, 30, 40 };
        v4si c = a + b;
        CHECK(c[0] == 11 && c[3] == 44, "vector add");
        pid = fork();
        if (pid == 0) {
            v4si d = c * a;                     /* the child's own copy of the registers */
            usleep(20000);
            _exit(d[3] == 176 ? 21 : 22);
        }
        for (int i = 0; i < 50; i++) { usleep(1000); c = c + a; }    /* switch back and forth while it computes */
        waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 21, "child's vector state");
        CHECK(c[0] == 61 && c[3] == 244, "parent's vector state after switches");
        got = 0;
        struct sigaction va = { .sa_handler = handler };
        sigaction(SIGUSR1, &va, NULL);
        kill(getpid(), SIGUSR1);                /* handler runs and (via libc) may use vector code */
        CHECK(got == SIGUSR1 && c[0] == 61 && c[3] == 244, "vector state across a signal handler");
    }
#endif

    printf("[test] mmap\n");
    /* mmap: executable anonymous memory (what tcc -run needs) and a file mapping */
    {
        unsigned char *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(code != MAP_FAILED, "mmap PROT_EXEC");
        if (code != MAP_FAILED) {
#ifdef __powerpc__
            /* li r3, 42 ; blr */
            unsigned char insn[] = { 0x38, 0x60, 0x00, 0x2a, 0x4e, 0x80, 0x00, 0x20 };
#else
            /* mov eax, 42 ; ret */
            unsigned char insn[] = { 0xb8, 0x2a, 0, 0, 0, 0xc3 };
#endif
            memcpy(code, insn, sizeof(insn));
            __builtin___clear_cache((char *)code, (char *)code + sizeof(insn));
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

    /* framebuffer: /dev/fb0 open, ioctl, and mmap */
    printf("[test] framebuffer\n");
    {
        int fb_fd = open("/dev/fb0", O_RDWR);
        if (fb_fd >= 0) {
            struct fb_var_screeninfo vinfo;
            struct fb_fix_screeninfo finfo;
            CHECK(ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0, "ioctl FBIOGET_VSCREENINFO");
            CHECK(ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) == 0, "ioctl FBIOGET_FSCREENINFO");
            CHECK(vinfo.xres > 0 && vinfo.yres > 0, "fb resolution valid");
            size_t sz = (size_t)finfo.line_length * vinfo.yres;
            void *fb_m = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
            CHECK(fb_m != MAP_FAILED, "fb mmap");
            if (fb_m != MAP_FAILED) {
                CHECK(munmap(fb_m, sz) == 0, "fb munmap");
            }
            close(fb_fd);
        } else {
            printf("  (no /dev/fb0 device, skipping fb test)\n");
        }
    }

    /* tcc -run: compile straight to memory and execute */
    if (have_tcc) {
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
    }

    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    usleep(20000);
    clock_gettime(CLOCK_MONOTONIC, &b);
    long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    CHECK(ms >= 20 && ms < 60, "usleep/clock_gettime");

    printf("[test] network\n");
    /* networking over loopback: UDP datagrams, poll, a TCP connection */
    {
        int u1 = socket(AF_INET, SOCK_DGRAM, 0), u2 = socket(AF_INET, SOCK_DGRAM, 0);
        CHECK(u1 >= 0 && u2 >= 0, "socket(AF_INET, SOCK_DGRAM)");
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(5555), .sin_addr.s_addr = htonl(0x7F000001) };
        CHECK(bind(u1, (struct sockaddr *)&a, sizeof a) == 0, "bind udp");
        struct pollfd pf = { u1, POLLIN, 0 };
        CHECK(poll(&pf, 1, 0) == 0, "poll: nothing to read yet");
        CHECK(sendto(u2, "ping", 4, 0, (struct sockaddr *)&a, sizeof a) == 4, "sendto over lo");
        CHECK(poll(&pf, 1, 2000) == 1 && (pf.revents & POLLIN), "poll: datagram arrived");
        char buf[64];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(u1, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        CHECK(n == 4 && memcmp(buf, "ping", 4) == 0, "recvfrom payload");
        CHECK(from.sin_addr.s_addr == htonl(0x7F000001) && from.sin_port != 0, "recvfrom source address");
        CHECK(sendto(u1, "pong", 4, 0, (struct sockaddr *)&from, fl) == 4, "reply to source");
        CHECK(recv(u2, buf, sizeof buf, 0) == 4 && memcmp(buf, "pong", 4) == 0, "reply received");
        close(u1); close(u2);

        int l = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in la = { .sin_family = AF_INET, .sin_port = htons(8088), .sin_addr.s_addr = INADDR_ANY };
        CHECK(bind(l, (struct sockaddr *)&la, sizeof la) == 0 && listen(l, 4) == 0, "tcp bind/listen");
        pid_t child = fork();
        if (child == 0) {
            int c = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in ca = { .sin_family = AF_INET, .sin_port = htons(8088), .sin_addr.s_addr = htonl(0x7F000001) };
            if (connect(c, (struct sockaddr *)&ca, sizeof ca) != 0) _exit(1);
            size_t bigsz = 100000;
            char *big = malloc(bigsz);
            for (size_t i = 0; i < bigsz; i++) big[i] = (char)(i * 7);
            size_t off = 0;
            while (off < bigsz) {
                ssize_t w = send(c, big + off, bigsz - off, 0);
                if (w <= 0) _exit(2);
                off += (size_t)w;
            }
            shutdown(c, SHUT_WR);
            char echo[16];
            ssize_t r = recv(c, echo, sizeof echo, 0);
            if (r != 6 || memcmp(echo, "thanks", 6) != 0) _exit(3);
            if (recv(c, echo, sizeof echo, 0) != 0) _exit(4);   /* EOF after the server closes */
            close(c);
            _exit(0);
        }
        struct sockaddr_in pa;
        socklen_t pl = sizeof pa;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int c = accept(l, (struct sockaddr *)&pa, &pl);
        CHECK(c >= 0 && pa.sin_addr.s_addr == htonl(0x7F000001), "accept");
        size_t total = 0;
        int ok = 1;
        for (;;) {
            char rb[4096];
            ssize_t r = recv(c, rb, sizeof rb, 0);
            if (r < 0) { ok = 0; break; }
            if (r == 0) break;
            for (ssize_t i = 0; i < r; i++)
                if (rb[i] != (char)((total + (size_t)i) * 7)) ok = 0;
            total += (size_t)r;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        CHECK(ok && total == 100000, "tcp: 100000 bytes received intact, then EOF");
        printf("[test] tcp: 100000 bytes over lo in %ld ms\n", (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
        CHECK(send(c, "thanks", 6, 0) == 6, "tcp: send after peer's FIN");
        close(c);
        int st;
        waitpid(child, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "tcp client side");
        close(l);
        int probe = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in na = { .sin_family = AF_INET, .sin_port = htons(8089), .sin_addr.s_addr = htonl(0x7F000001) };
        CHECK(connect(probe, (struct sockaddr *)&na, sizeof na) < 0 && errno == ECONNREFUSED, "connect to closed port refused");
        close(probe);
        printf("[test] loopback udp/tcp/poll ok\n");
    }

    printf("[test] done: %d failure(s)\n", fails);
    return fails;
}
