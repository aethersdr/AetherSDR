#define _GNU_SOURCE
/* Manual radio admission probe. No installer, signed release or private
 * credential is included. The sibling requester is the production binary. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/capability.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    (void)argv;
    alarm(90);
    if (argc != 1 || !getuid() || getuid() != geteuid() || getgid() != getegid()) { return 2; }
    struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct caps[2];
    if (syscall(SYS_capget, &header, caps)) { return 2; }
    for (unsigned i = 0; i < 2; ++i) {
        if (caps[i].effective || caps[i].permitted || caps[i].inheritable) { return 2; }
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) { return 2; }
    char path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0 || n >= (ssize_t)sizeof(path) - 1) { return 3; }
    path[n] = 0;
    char* end = strrchr(path, '/');
    if (!end || strcmp(end, "/pd-first-install-probe")) { return 3; }
    *end = 0;
    if (chdir(path)) { return 3; }
    struct stat config;
    if (lstat("config", &config) || !S_ISREG(config.st_mode)) { return 3; }
    int fd = open("config", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    int readable = fd >= 0, open_error = fd < 0 ? errno : 0;
    if (fd >= 0) { close(fd); }
    pid_t parent = getpid(), child = fork();
    if (child < 0) { return 4; }
    if (!child) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) { _exit(126); }
        char* args[] = {"pd-request", NULL};
        char* env[] = {NULL};
        execve("./pd-request", args, env);
        _exit(127);
    }
    int status = 0, done = 0;
    for (unsigned i = 0; i < 200; ++i) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) { done = 1; break; }
        if (result < 0 && errno != EINTR) { break; }
        struct timespec delay = {0, 10000000};
        nanosleep(&delay, NULL);
    }
    if (!done) {
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
    char report[768];
    int length = snprintf(report, sizeof(report),
        "{\"application\":\"AetherPDFirstInstallProbe\",\"uid\":%u,\"gid\":%u,\"capabilities_empty\":true,\"no_new_privileges\":true,"
        "\"config_uid\":%u,\"config_gid\":%u,\"config_mode\":\"%04o\","
        "\"config_readable\":%s,\"config_open_errno\":%d,\"requester_exit\":%d,"
        "\"requester_timed_out\":%s,\"installed_shim_used\":false,"
        "\"installation_attempted\":false}\n",
        (unsigned)getuid(), (unsigned)getgid(), (unsigned)config.st_uid, (unsigned)config.st_gid,
        (unsigned)(config.st_mode & 07777), readable ? "true" : "false", open_error,
        done && WIFEXITED(status) ? WEXITSTATUS(status) : -1, done ? "false" : "true");
    if (length <= 0 || length >= (int)sizeof(report)) { return 4; }
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) { return 4; }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(49183);
    if (bind(listener, (struct sockaddr*)&address, sizeof(address)) || listen(listener, 1)) { close(listener); return 4; }
    struct pollfd waiting = {listener, POLLIN, 0};
    int connected = poll(&waiting, 1, 60000) == 1 ? accept4(listener, NULL, NULL, SOCK_CLOEXEC) : -1;
    close(listener);
    if (connected < 0) { return 4; }
    struct timeval timeout = {2, 0};
    setsockopt(connected, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    ssize_t sent = send(connected, report, (size_t)length, MSG_NOSIGNAL);
    close(connected);
    return sent == length ? 0 : 4;
}
