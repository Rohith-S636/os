#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <signal.h>
#include <sched.h>
#include <sys/mount.h>
#include <stdlib.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>

#define MAX_CONTAINERS 10
#define STACK_SIZE (1024 * 1024)
#define BUFFER_SIZE 10
#define SOCKET_PATH "/tmp/engine.sock"

#define IOCTL_REGISTER_PID _IOW('a', 'a', int)

// ================= BUFFER =================
typedef struct {
    char data[BUFFER_SIZE][256];
    int in, out, count;
    pthread_mutex_t mutex;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
} buffer_t;

buffer_t buffer;

// ================= CONTAINER =================
typedef struct {
    int id;
    pid_t pid;
    char status[20];
    int pipe_fd;
} container_t;

container_t containers[MAX_CONTAINERS];
int count = 0;

char stack[STACK_SIZE];

// ================= BUFFER INIT =================
void init_buffer() {
    buffer.in = buffer.out = buffer.count = 0;
    pthread_mutex_init(&buffer.mutex, NULL);
    pthread_cond_init(&buffer.not_full, NULL);
    pthread_cond_init(&buffer.not_empty, NULL);
}

// ================= PRODUCER =================
void* producer(void* arg) {
    int fd = *(int*)arg;
    free(arg);  // 🔥 FIXED

    char temp[256];
    int n;

    while ((n = read(fd, temp, sizeof(temp)-1)) > 0) {
        temp[n] = '\0';

        pthread_mutex_lock(&buffer.mutex);

        while (buffer.count == BUFFER_SIZE)
            pthread_cond_wait(&buffer.not_full, &buffer.mutex);

        strcpy(buffer.data[buffer.in], temp);
        buffer.in = (buffer.in + 1) % BUFFER_SIZE;
        buffer.count++;

        pthread_cond_signal(&buffer.not_empty);
        pthread_mutex_unlock(&buffer.mutex);
    }

    close(fd);
    return NULL;
}

// ================= CONSUMER =================
void* consumer(void* arg) {
    system("mkdir -p logs");

    FILE *log = fopen("logs/container.log", "a");
    if (!log) {
        perror("fopen failed");
        return NULL;
    }

    while (1) {
        pthread_mutex_lock(&buffer.mutex);

        while (buffer.count == 0)
            pthread_cond_wait(&buffer.not_empty, &buffer.mutex);

        char temp[256];
        strcpy(temp, buffer.data[buffer.out]);

        buffer.out = (buffer.out + 1) % BUFFER_SIZE;
        buffer.count--;

        pthread_cond_signal(&buffer.not_full);
        pthread_mutex_unlock(&buffer.mutex);

        fprintf(log, "%s", temp);
        fflush(log);
    }
}

// ================= CONTAINER =================
int container_func(void *arg) {
    int *pipefd = (int *)arg;

    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);

    close(pipefd[0]);
    close(pipefd[1]);

    sethostname("mycontainer", 11);

    if (chroot("./rootfs") != 0) {
        perror("chroot failed");
        return 1;
    }

    chdir("/");
    mount("proc", "/proc", "proc", 0, NULL);

    // Debug + stress
    execl("/bin/sh", "sh", "-c", "echo HELLO; /stress", NULL);

    perror("exec failed");
    return 1;
}

// ================= SIGCHLD =================
void sigchld_handler(int sig) {
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < count; i++) {
            if (containers[i].pid == pid) {
                strcpy(containers[i].status, "stopped");
                break;
            }
        }
    }
}

// ================= START =================
void start_container() {
    int pipefd[2];
    pipe(pipefd);

    pid_t pid = clone(container_func,
                      stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      pipefd);

    if (pid == -1) {
        perror("clone failed");
        return;
    }

    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd >= 0) {
        ioctl(fd, IOCTL_REGISTER_PID, &pid);
        close(fd);
    }

    containers[count].id = count;
    containers[count].pid = pid;
    strcpy(containers[count].status, "running");
    containers[count].pipe_fd = pipefd[0];
    count++;

    close(pipefd[1]);

    // 🔥 FIXED PRODUCER ARGUMENT
    int *fd_ptr = malloc(sizeof(int));
    *fd_ptr = pipefd[0];

    pthread_t prod;
    pthread_create(&prod, NULL, producer, fd_ptr);
    pthread_detach(prod);
}

// ================= STOP =================
void stop_container(int id) {
    if (id >= count) return;

    kill(containers[id].pid, SIGKILL);
    strcpy(containers[id].status, "stopped");
}

// ================= PS =================
void list_containers(int client_fd) {
    char buf[512];
    int len = sprintf(buf, "ID\tPID\tSTATUS\n");

    for (int i = 0; i < count; i++) {
        len += sprintf(buf + len, "%d\t%d\t%s\n",
                       containers[i].id,
                       containers[i].pid,
                       containers[i].status);
    }

    write(client_fd, buf, strlen(buf));
}

// ================= LOGS =================
void send_logs(int client_fd) {
    FILE *f = fopen("logs/container.log", "r");
    if (!f) {
        write(client_fd, "No logs available\n", 18);
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        write(client_fd, line, strlen(line));
    }

    fclose(f);
}

// ================= SUPERVISOR =================
void run_supervisor() {
    int server_fd, client_fd;
    struct sockaddr_un addr;
    char cmd[256];

    unlink(SOCKET_PATH);

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);

    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 5);

    printf("Supervisor running...\n");

    while (1) {
        client_fd = accept(server_fd, NULL, NULL);

        int n = read(client_fd, cmd, sizeof(cmd));
        cmd[n] = '\0';

        if (strncmp(cmd, "start", 5) == 0) {
            start_container();
            write(client_fd, "Started\n", 8);
        }
        else if (strncmp(cmd, "ps", 2) == 0) {
            list_containers(client_fd);
        }
        else if (strncmp(cmd, "stop", 4) == 0) {
            int id = atoi(cmd + 5);
            stop_container(id);
            write(client_fd, "Stopped\n", 8);
        }
        else if (strncmp(cmd, "logs", 4) == 0) {
            send_logs(client_fd);
        }
        else {
            write(client_fd, "Unknown\n", 8);
        }

        close(client_fd);
    }
}

// ================= CLIENT =================
void send_command(int argc, char *argv[]) {
    int sock;
    struct sockaddr_un addr;
    char buffer[256] = {0};

    sock = socket(AF_UNIX, SOCK_STREAM, 0);

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("connect");
        return;
    }

    for (int i = 1; i < argc; i++) {
        strcat(buffer, argv[i]);
        strcat(buffer, " ");
    }

    write(sock, buffer, strlen(buffer));

    int n = read(sock, buffer, sizeof(buffer));
    buffer[n] = '\0';

    printf("%s", buffer);

    close(sock);
}

// ================= MAIN =================
int main(int argc, char *argv[]) {

    signal(SIGCHLD, sigchld_handler);
    init_buffer();

    pthread_t cons;
    pthread_create(&cons, NULL, consumer, NULL);
    pthread_detach(cons);

    if (argc < 2) {
        printf("Usage:\n");
        printf("  engine supervisor\n");
        printf("  engine start\n");
        printf("  engine ps\n");
        printf("  engine stop <id>\n");
        printf("  engine logs\n");
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        run_supervisor();
    } else {
        send_command(argc, argv);
    }

    return 0;
}
