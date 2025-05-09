#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#define PORT_NUM     1004
#define NAME_LEN     32
#define MSG_LEN      512
#define MAX_CLIENTS  20    // max total clients
#define MAX_ROOMS    10    // max rooms supported

void error(const char *msg) {
    perror(msg);
    exit(1);
}

typedef struct {
    int  clisockfd;
    char name[NAME_LEN];
    int  color;
    int  valid;
    char ip[INET_ADDRSTRLEN];
    int  room_id;
} ThreadArgs;

static ThreadArgs *clients[MAX_CLIENTS] = {0};
static pthread_mutex_t clients_mtx = PTHREAD_MUTEX_INITIALIZER;

//─── Utility: print all connected users ───────────────────────────────────────
void print_connected_users() {
    printf("Connected Users:");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        ThreadArgs *c = clients[i];
        if (c && c->valid) {
            printf(" %s(%s)[room %d]", c->name, c->ip, c->room_id);
        }
    }
    printf("\n");
}

//─── Ignore SIGPIPE so send() errors don’t kill us ─────────────────────────────
void setup_signal_handler() {
    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGPIPE, &sa, NULL);
}

//─── Safe send to avoid SIGPIPE crashes ───────────────────────────────────────
int safe_send(int sockfd, const char *msg, size_t len) {
    if (sockfd < 0) return -1;
    int ret = send(sockfd, msg, len, MSG_NOSIGNAL);
    if (ret < 0) {
        fprintf(stderr, "send() to fd %d failed: %s\n", sockfd, strerror(errno));
    }
    return ret;
}

//─── Check if any client occupies the given room ──────────────────────────────
int room_has_clients(int room_id) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        ThreadArgs *c = clients[i];
        if (c && c->valid && c->room_id == room_id)
            return 1;
    }
    return 0;
}

//─── Find the lowest free room ID in [1..MAX_ROOMS] ───────────────────────────
int allocate_new_room() {
    for (int r = 1; r <= MAX_ROOMS; r++) {
        if (!room_has_clients(r)) return r;
    }
    return -1;
}

//─── Add client to global list & print users ──────────────────────────────────
void register_client(ThreadArgs *c) {
    pthread_mutex_lock(&clients_mtx);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i]) {
            clients[i] = c;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mtx);

    print_connected_users();
}

//─── Remove client from global list & print users ─────────────────────────────
void deregister_client(ThreadArgs *c) {
    pthread_mutex_lock(&clients_mtx);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] == c) {
            clients[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mtx);

    print_connected_users();
}

//─── Broadcast a message to everyone in the same room ─────────────────────────
void broadcast_room(const char *msg, int room_id) {
    pthread_mutex_lock(&clients_mtx);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        ThreadArgs *c = clients[i];
        if (c && c->valid && c->room_id == room_id) {
            if (safe_send(c->clisockfd, msg, strlen(msg)) < 0) {
                c->valid = 0;
            }
        }
    }
    // clean up disconnected
    for (int i = 0; i < MAX_CLIENTS; i++) {
        ThreadArgs *c = clients[i];
        if (c && !c->valid) {
            close(c->clisockfd);
            free(c);
            clients[i] = NULL;
        }
    }
    pthread_mutex_unlock(&clients_mtx);
}

//─── Per-client thread: handle username, chat, join/leave ────────────────────
void *handle_client(void *arg) {
    pthread_detach(pthread_self());
    ThreadArgs *cli = arg;
    char buf[MSG_LEN];
    int  n;

    cli->valid = 1;
    strcpy(cli->name, "Anonymous");

    // ask for username
    safe_send(cli->clisockfd, "Type your user name: ", 21);

    // receive username
    n = recv(cli->clisockfd, buf, NAME_LEN - 1, 0);
    if (n <= 0) goto CLEANUP;
    buf[n] = '\0';
    if (buf[n-1] == '\n') buf[n-1] = '\0';
    strncpy(cli->name, buf, NAME_LEN - 1);

    // pick a random ANSI color
    cli->color = 31 + (rand() % 7);

    // register & announce join
    register_client(cli);
    snprintf(buf, sizeof(buf),
        "\033[1;%dm%s (%s) joined room %d!\033[0m\n",
        cli->color, cli->name, cli->ip, cli->room_id);
    broadcast_room(buf, cli->room_id);

    // chat loop
    while ((n = recv(cli->clisockfd, buf, MSG_LEN - 1, 0)) > 0) {
        buf[n] = '\0';
        char out[MSG_LEN + 64];
        if (buf[strlen(buf)-1] != '\n') {
            snprintf(out, sizeof(out),
                "\033[1;%dm[%s (%s)]\033[0m %s\n",
                cli->color, cli->name, cli->ip, buf);
        } else {
            snprintf(out, sizeof(out),
                "\033[1;%dm[%s (%s)]\033[0m %s",
                cli->color, cli->name, cli->ip, buf);
        }
        broadcast_room(out, cli->room_id);
    }

    // announce leave
    snprintf(buf, sizeof(buf),
        "\033[1;%dm%s (%s) left room %d!\033[0m\n",
        cli->color, cli->name, cli->ip, cli->room_id);
    broadcast_room(buf, cli->room_id);

CLEANUP:
    deregister_client(cli);
    close(cli->clisockfd);
    free(cli);
    return NULL;
}

//─── Main: set up server, accept room requests, spawn threads ───────────────
int main() {
    // disable stdout buffering so prints appear immediately
    setvbuf(stdout, NULL, _IONBF, 0);

    setup_signal_handler();
    srand((unsigned)time(NULL));

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) error("socket");

    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv = {0};
    serv.sin_family      = AF_INET;
    serv.sin_addr.s_addr = INADDR_ANY;
    serv.sin_port        = htons(PORT_NUM);

    if (bind(listenfd, (struct sockaddr*)&serv, sizeof(serv)) < 0)
        error("bind");

    listen(listenfd, 5);
    printf("Server listening on port %d\n", PORT_NUM);

    while (1) {
        struct sockaddr_in cliaddr;
        socklen_t len = sizeof(cliaddr);
        int fd = accept(listenfd, (struct sockaddr*)&cliaddr, &len);
        if (fd < 0) continue;

        // read initial room request: "new" or "<room#>"
        char req[16];
        int r = recv(fd, req, sizeof(req)-1, 0);
        if (r <= 0) { close(fd); continue; }
        req[r] = '\0';
        if (req[r-1]=='\n') req[r-1] = '\0';

        // allocate and populate ThreadArgs
        ThreadArgs *cli = calloc(1, sizeof(*cli));
        cli->clisockfd = fd;
        inet_ntop(AF_INET, &cliaddr.sin_addr, cli->ip, sizeof(cli->ip));

        // decide room
        int room_id = -1;
        if (strcmp(req, "new") == 0) {
            room_id = allocate_new_room();
            if (room_id < 0) {
                safe_send(fd, "Server: no more rooms available\n", 32);
                close(fd);
                free(cli);
                continue;
            }
            char resp[64];
            snprintf(resp, sizeof(resp),
                "Connected to %s with new room number %d\n",
                inet_ntoa(cliaddr.sin_addr), room_id);
            safe_send(fd, resp, strlen(resp));
        } else {
            int rid = atoi(req);
            if (rid < 1 || rid > MAX_ROOMS || !room_has_clients(rid)) {
                safe_send(fd, "Server: invalid room number\n", 28);
                close(fd);
                free(cli);
                continue;
            }
            room_id = rid;
            char resp[64];
            snprintf(resp, sizeof(resp),
                "Connected to %s with room number %d\n",
                inet_ntoa(cliaddr.sin_addr), room_id);
            safe_send(fd, resp, strlen(resp));
        }
        cli->room_id = room_id;

        // spawn handler thread
        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, cli) != 0) {
            close(fd);
            free(cli);
        }
    }

    close(listenfd);
    return 0;
}
