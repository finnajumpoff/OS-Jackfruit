/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 10

/* ... rest of defines ... */
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * Task 2: Logging Implementation (Bounded Buffer)
 *
 * Requirements:
 *   - block when the buffer is full (count == LOG_BUFFER_CAPACITY)
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * Task 2: Logging Consumer Thread
 *
 * Responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    bounded_buffer_t *buffer = (bounded_buffer_t *)arg;
    log_item_t item;

    // Ensure log directory exists
    mkdir(LOG_DIR, 0755);

    while (bounded_buffer_pop(buffer, &item) == 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            if (write(fd, item.data, item.length) < 0) {
                perror("write log");
            }
            close(fd);
        }
    }

    return NULL;
}

/*
 * Task 1: Runtime Implementation
 *
 * Requirements:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *config = (child_config_t *)arg;

    // 1. Unshare namespaces: PID, UTS, and Mount
    // This provides isolation for the container's processes, hostname, and filesystem.
    if (unshare(CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWPID) < 0) {
        perror("unshare");
        return 1;
    }

    // 2. Fork to enter the new PID namespace. 
    // unshare(CLONE_NEWPID) affects the children of the calling process, not the process itself.
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid > 0) {
        // First child: wait for the second child (PID 1 in the new namespace) to finish.
        int status;
        if (waitpid(pid, &status, 0) < 0) {
            perror("waitpid");
            return 1;
        }
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        return 1;
    }

    // Second child (Grandchild): This is the container process (PID 1 in its namespace).

    // 3. UTS isolation: set the container hostname.
    if (sethostname(config->id, strlen(config->id)) < 0) {
        perror("sethostname");
    }

    // 4. Resource control: set nice value if requested.
    if (config->nice_value != 0) {
        if (setpriority(PRIO_PROCESS, 0, config->nice_value) < 0) {
            perror("setpriority");
        }
    }

    // 5. Output redirection: connect stdout and stderr to the supervisor's log pipe.
    if (config->log_write_fd >= 0) {
        if (dup2(config->log_write_fd, STDOUT_FILENO) < 0) perror("dup2 stdout");
        if (dup2(config->log_write_fd, STDERR_FILENO) < 0) perror("dup2 stderr");
        close(config->log_write_fd);
    }

    // 6. Mount isolation: ensure mounts don't propagate back to the host.
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        perror("mount propagation");
    }

    // 7. Filesystem isolation: chroot into the provided rootfs.
    if (chroot(config->rootfs) < 0) {
        perror("chroot");
        exit(1);
    }
    if (chdir("/") < 0) {
        perror("chdir");
        exit(1);
    }

    // 8. Mount /proc: required for tools like 'ps' to work within the PID namespace.
    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        perror("mount proc");
        exit(1);
    }

    // 9. Execute the container command.
    char *argv_child[] = { "/bin/sh", "-c", config->command, NULL };
    execv(argv_child[0], argv_child);

    // If execv returns, it failed.
    perror("execv");
    exit(1);
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

static volatile sig_atomic_t stop_supervisor = 0;

static void handle_signal(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        stop_supervisor = 1;
    }
}

static void handle_sigchld(int sig)
{
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

static void add_container_record(supervisor_ctx_t *ctx, container_record_t *rec)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static container_record_t *find_container_record(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *curr;
    pthread_mutex_lock(&ctx->metadata_lock);
    curr = ctx->containers;
    while (curr) {
        if (strcmp(curr->id, id) == 0) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            return curr;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
    return NULL;
}


typedef struct {
    int read_fd;
    char id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;
} pipe_reader_args_t;

static void *pipe_reader_thread(void *arg) {
    pipe_reader_args_t *args = (pipe_reader_args_t *)arg;
    log_item_t item;
    strncpy(item.container_id, args->id, sizeof(item.container_id));
    ssize_t n;
    while ((n = read(args->read_fd, item.data, sizeof(item.data))) > 0) {
        item.length = (size_t)n;
        bounded_buffer_push(args->buffer, &item);
    }
    close(args->read_fd);
    free(args);
    return NULL;
}

/*
 * Task 3: Supervisor Implementation
 *
 * Responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;
    struct sockaddr_un addr;

    (void)rootfs; // Base rootfs can be used for global config if needed

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) {
        perror("open /dev/container_monitor (Task 4 required for full features)");
    }

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    // 1. Create control socket
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    unlink(CONTROL_PATH);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(ctx.server_fd, 5) < 0) {
        perror("listen");
        return 1;
    }

    // 2. Install signal handlers
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct sigaction sa_chld;
    sa_chld.sa_handler = handle_sigchld;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    // 3. Spawn logger thread
    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx.log_buffer) != 0) {
        perror("pthread_create logger");
        return 1;
    }

    printf("Supervisor running. Control socket: %s\n", CONTROL_PATH);

    // 4. Main event loop
    while (!stop_supervisor) {
        struct timeval tv = {1, 0};
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(ctx.server_fd, &readfds);

        int n = select(ctx.server_fd + 1, &readfds, NULL, NULL, &tv);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (n > 0 && FD_ISSET(ctx.server_fd, &readfds)) {
            int client_fd = accept(ctx.server_fd, NULL, NULL);
            if (client_fd < 0) {
                perror("accept");
                continue;
            }

            control_request_t req;
            if (recv(client_fd, &req, sizeof(req), 0) <= 0) {
                close(client_fd);
                continue;
            }

            control_response_t res;
            res.status = 0;
            strcpy(res.message, "OK");

            if (req.kind == CMD_START || req.kind == CMD_RUN) {
                if (find_container_record(&ctx, req.container_id)) {
                    res.status = -1;
                    strcpy(res.message, "Container ID already exists");
                } else {
                    int pipe_fds[2];
                    if (pipe(pipe_fds) < 0) {
                        res.status = -1;
                        strcpy(res.message, "pipe failed");
                    } else {
                        child_config_t *config = malloc(sizeof(child_config_t));
                        strncpy(config->id, req.container_id, sizeof(config->id));
                        strncpy(config->rootfs, req.rootfs, sizeof(config->rootfs));
                        strncpy(config->command, req.command, sizeof(config->command));
                        config->nice_value = req.nice_value;
                        config->log_write_fd = pipe_fds[1];

                        void *stack = malloc(STACK_SIZE);
                        pid_t pid = clone(child_fn, (char *)stack + STACK_SIZE, SIGCHLD, config);
                        
                        if (pid < 0) {
                            res.status = -1;
                            strcpy(res.message, "clone failed");
                            free(stack);
                            free(config);
                            close(pipe_fds[0]);
                            close(pipe_fds[1]);
                        } else {
                            container_record_t *rec = malloc(sizeof(container_record_t));
                            strncpy(rec->id, req.container_id, sizeof(rec->id));
                            rec->host_pid = pid;
                            rec->started_at = time(NULL);
                            rec->state = CONTAINER_RUNNING;
                            rec->soft_limit_bytes = req.soft_limit_bytes;
                            rec->hard_limit_bytes = req.hard_limit_bytes;
                            add_container_record(&ctx, rec);

                            if (ctx.monitor_fd >= 0) {
                                register_with_monitor(ctx.monitor_fd, rec->id, pid, 
                                                    rec->soft_limit_bytes, rec->hard_limit_bytes);
                            }

                            pipe_reader_args_t *pargs = malloc(sizeof(pipe_reader_args_t));
                            pargs->read_fd = pipe_fds[0];
                            strncpy(pargs->id, req.container_id, sizeof(pargs->id));
                            pargs->buffer = &ctx.log_buffer;

                            pthread_t pt;
                            pthread_create(&pt, NULL, pipe_reader_thread, pargs);
                            pthread_detach(pt);
                            
                            close(pipe_fds[1]); // Close write end in supervisor
                        }
                    }
                }
            } else if (req.kind == CMD_STOP) {
                container_record_t *rec = find_container_record(&ctx, req.container_id);
                if (rec) {
                    kill(rec->host_pid, SIGTERM);
                    rec->state = CONTAINER_STOPPED;
                } else {
                    res.status = -1;
                    strcpy(res.message, "Container not found");
                }
            } else if (req.kind == CMD_PS) {
                pthread_mutex_lock(&ctx.metadata_lock);
                container_record_t *curr = ctx.containers;
                res.message[0] = '\0';
                while (curr) {
                    char line[64];
                    snprintf(line, sizeof(line), "%s %d %s\n", curr->id, curr->host_pid, state_to_string(curr->state));
                    strncat(res.message, line, sizeof(res.message) - strlen(res.message) - 1);
                    curr = curr->next;
                }
                pthread_mutex_unlock(&ctx.metadata_lock);
            }

            send(client_fd, &res, sizeof(res), 0);
            close(client_fd);
        }
    }

    printf("Supervisor shutting down...\n");
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    if (ctx.server_fd >= 0) close(ctx.server_fd);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    unlink(CONTROL_PATH);

    return 0;
}

/*
 * Task 3: Client Control Path
 */
static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t res;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is the supervisor running?)");
        close(fd);
        return 1;
    }

    if (send(fd, req, sizeof(*req), 0) < 0) {
        perror("send");
        close(fd);
        return 1;
    }

    if (recv(fd, &res, sizeof(res), 0) < 0) {
        perror("recv");
        close(fd);
        return 1;
    }

    if (req->kind == CMD_PS) {
        printf("ID PID STATE\n%s", res.message);
    } else {
        printf("Response: %s (status: %d)\n", res.message, res.status);
    }

    close(fd);
    return res.status;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    /*
     * TODO:
     * The supervisor should respond with container metadata.
     * Keep the rendering format simple enough for demos and debugging.
     */
    printf("Expected states include: %s, %s, %s, %s, %s\n",
           state_to_string(CONTAINER_STARTING),
           state_to_string(CONTAINER_RUNNING),
           state_to_string(CONTAINER_STOPPED),
           state_to_string(CONTAINER_KILLED),
           state_to_string(CONTAINER_EXITED));
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
