#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <setjmp.h>
#include <errno.h>

#define MAX_LINE 1024
#define MAX_ARGS 64
#define MAX_BG   128

static volatile sig_atomic_t fg_pid = -1;       // 当前前台进程 pid
static volatile sig_atomic_t timeout_pid = -1;  // timeout 正在监控的子进程 pid
static pid_t bg_pids[MAX_BG];                   // 后台作业 pid 表
static sigjmp_buf timeout_env;                  // 超时跳转环境

/* ---------- 信号处理函数 ---------- */

static void sigchld_handler(int sig) {
    int saved_errno = errno;
    int status;
    pid_t ret;

    // 只回收后台作业，避免抢走前台 waitpid 要回收的子进程
    for (int i = 0; i < MAX_BG; i++) {
        if (bg_pids[i] > 0) {
            ret = waitpid(bg_pids[i], &status, WNOHANG);
            if (ret == bg_pids[i] || (ret == -1 && errno == ECHILD)) {
                bg_pids[i] = -1;
            }
        }
    }
    errno = saved_errno;
}

static void sigint_handler(int sig) {
    // Ctrl+C：只终止当前前台作业，Shell 本身不退出
    if (fg_pid > 0) {
        kill(fg_pid, SIGINT);
    }
}

static void sigalrm_handler(int sig) {
    // 超时：杀掉 timeout 监控的子进程，然后跳回 setjmp 点
    if (timeout_pid > 0) {
        kill(timeout_pid, SIGKILL);
    }
    siglongjmp(timeout_env, 1);
}

/* ---------- 工具函数 ---------- */

static void add_bg_pid(pid_t pid) {
    for (int i = 0; i < MAX_BG; i++) {
        if (bg_pids[i] <= 0) {
            bg_pids[i] = pid;
            return;
        }
    }
    // 后台表满了就简单忽略，实验够用
}

static int parse_line(char *line, char **argv, int *background) {
    int argc = 0;
    *background = 0;

    char *p = line;
    while (*p && argc < MAX_ARGS - 1) {
        // 跳过空白
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (*p == '\0') break;

        // 遇到 & 表示后台运行
        if (*p == '&') {
            *background = 1;
            break;
        }

        argv[argc++] = p;
        // 找到参数结尾
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (*p) {
            *p = '\0';
            p++;
        }
    }
    argv[argc] = NULL;
    return argc;
}

static int builtin_type(char **argv) {
    if (!argv[0]) return 0;
    if (strcmp(argv[0], "exit") == 0)    return 1;
    if (strcmp(argv[0], "cd") == 0)      return 2;
    if (strcmp(argv[0], "timeout") == 0) return 3;
    return 0;
}

/* ---------- 执行外部命令 ---------- */

static void run_external(char **argv, int background) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        // 子进程恢复默认信号，确保 Ctrl+C 能杀死它
        signal(SIGINT, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGALRM, SIG_DFL);

        execvp(argv[0], argv);
        perror("execvp");
        _exit(127);
    }

    if (background) {
        add_bg_pid(pid);
        printf("[bg] %d\n", pid);
    } else {
        fg_pid = pid;
        int status;
        // waitpid 可能被信号中断，循环处理 EINTR
        while (waitpid(pid, &status, 0) < 0) {
            if (errno == EINTR) continue;
            perror("waitpid");
            break;
        }
        fg_pid = -1;
    }
}

/* ---------- 内置 timeout ---------- */

static void run_timeout(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: timeout <sec> <cmd> [args...]\n");
        return;
    }

    int sec = atoi(argv[1]);
    if (sec <= 0) {
        printf("timeout: sec must be > 0\n");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGALRM, SIG_DFL);
        execvp(argv[2], &argv[2]);
        perror("execvp");
        _exit(127);
    }

    // 父进程：设置超时监控
    timeout_pid = pid;

    if (sigsetjmp(timeout_env, 1) == 0) {
        alarm(sec);                 // 设置定时器
        int status;
        while (waitpid(pid, &status, 0) < 0) {
            if (errno == EINTR) continue;
            perror("waitpid");
            break;
        }
        alarm(0);                   // 正常结束，取消定时器
        timeout_pid = -1;
    } else {
        // 从 SIGALRM handler 跳回这里，说明超时
        int status;
        waitpid(pid, &status, 0);   // 回收被 SIGKILL 的子进程
        timeout_pid = -1;
        printf("Timeout!\n");
    }
}

/* ---------- main ---------- */

int main(void) {
    struct sigaction sa;

    // 初始化后台 pid 表
    for (int i = 0; i < MAX_BG; i++) bg_pids[i] = -1;

    // 注册 SIGCHLD
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = 0;
    sigaction(SIGCHLD, &sa, NULL);

    // 注册 SIGINT
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = sigint_handler;
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    // 注册 SIGALRM
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = sigalrm_handler;
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, NULL);

    char line[MAX_LINE];
    char *argv[MAX_ARGS];
    int background;

    while (1) {
        printf("msh> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }

        int argc = parse_line(line, argv, &background);
        if (argc == 0) continue;

        int type = builtin_type(argv);
        if (type == 1) {            // exit
            break;
        } else if (type == 2) {     // cd
            if (argc > 1) {
                if (chdir(argv[1]) != 0) perror("cd");
            } else {
                char *home = getenv("HOME");
                if (home) chdir(home);
            }
        } else if (type == 3) {     // timeout
            run_timeout(argc, argv);
        } else {                    // 外部命令
            run_external(argv, background);
        }
    }

    return 0;
}