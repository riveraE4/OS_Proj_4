// chat_server.c

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

#define PORT_NUM     15000
#define NAME_LEN     32
#define MSG_LEN      512
#define MAX_CLIENTS  20
#define MAX_ROOMS    10

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

// ── Helper: count how many clients are in a given room ───────────────────────
static int count_clients_in_room(int room_id) {
    int cnt = 0;
    pthread_mutex_lock(&clients_mtx);
      for (int i = 0; i < MAX_CLIENTS; i++) {
          ThreadArgs *c = clients[i];
          if (c && c->valid && c->room_id == room_id)
              cnt++;
      }
    pthread_mutex_unlock(&clients_mtx);
    return cnt;
}

void error(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

void setup_signal_handler() {
    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGPIPE, &sa, NULL);
}

int safe_send(int sockfd, const char *msg, size_t len) {
    if (sockfd < 0) return -1;
    int ret = send(sockfd, msg, len, MSG_NOSIGNAL);
    if (ret < 0)
        fprintf(stderr, "send() to fd %d failed: %s\n", sockfd, strerror(errno));
    return ret;
}

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

int allocate_new_room() {
    for (int r = 1; r <= MAX_ROOMS; r++) {
        if (count_clients_in_room(r) == 0) return r;
    }
    return -1;
}

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

void broadcast_room(const char *msg, int room_id) {
    pthread_mutex_lock(&clients_mtx);
      for (int i = 0; i < MAX_CLIENTS; i++) {
          ThreadArgs *c = clients[i];
          if (c && c->valid && c->room_id == room_id) {
              if (safe_send(c->clisockfd, msg, strlen(msg)) < 0)
                  c->valid = 0;
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

void *handle_client(void *arg) {
    pthread_detach(pthread_self());
    ThreadArgs *cli = arg;
    char buf[MSG_LEN];
    int  n;

    cli->valid = 1;
    strcpy(cli->name, "Anonymous");

    // ask for username
    safe_send(cli->clisockfd,
              "Type your user name: ",
              strlen("Type your user name: "), 0);

    // receive username
    n = recv(cli->clisockfd, buf, NAME_LEN - 1, 0);
    if (n <= 0) goto CLEANUP;
    buf[n] = '\0';
    if (buf[n-1] == '\n') buf[n-1] = '\0';
    strncpy(cli->name, buf, NAME_LEN - 1);

    // pick a random color
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
        snprintf(out, sizeof(out),
                 "\033[1;%dm[%s (%s)]\033[0m %s\n",
                 cli->color, cli->name, cli->ip, buf);
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

int main() {
    int listenfd;
    struct sockaddr_in serv, cliaddr;

    setvbuf(stdout, NULL, _IONBF, 0);
    setup_signal_handler();
    srand((unsigned)time(NULL));

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) error("socket");

    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&serv, 0, sizeof(serv));
    serv.sin_family      = AF_INET;
    serv.sin_addr.s_addr = INADDR_ANY;
    serv.sin_port        = htons(PORT_NUM);

    if (bind(listenfd, (struct sockaddr*)&serv, sizeof(serv)) < 0)
        error("bind");

    listen(listenfd, 5);
    printf("Server listening on port %d\n", PORT_NUM);

    while (1) {
        socklen_t len = sizeof(cliaddr);
        int fd = accept(listenfd, (struct sockaddr*)&cliaddr, &len);
        if (fd < 0) continue;

        // receive initial request (could be "" if client sent "\n")
        char req[16];
        int r = recv(fd, req, sizeof(req)-1, 0);
        if (r <= 0) { close(fd); continue; }
        req[r] = '\0';
        if (req[r-1]=='\n') req[r-1] = '\0';

        ThreadArgs *cli = calloc(1, sizeof(*cli));
        cli->clisockfd = fd;
        inet_ntop(AF_INET, &cliaddr.sin_addr, cli->ip, sizeof(cli->ip));

        // === simplified Checkpoint 3 handshake ===
        int room_id = -1;
        if (req[0] == '\0') {
            // client blank → check if any rooms exist
            int any = 0;
            for (int i = 1; i <= MAX_ROOMS; i++) {
                if (count_clients_in_room(i) > 0) { any = 1; break; }
            }

            if (!any) {
                // no rooms → auto-create
                room_id = allocate_new_room();
                char resp[64];
                snprintf(resp, sizeof(resp),
                         "Connected to %s with new room number %d\n",
                         inet_ntoa(cliaddr.sin_addr), room_id);
                safe_send(fd, resp, strlen(resp));
            } else {
                // send menu
                safe_send(fd,
                    "Server says following options are available:\n",
                    strlen("Server says following options are available:\n"));
                for (int i = 1; i <= MAX_ROOMS; i++) {
                    int cnt = count_clients_in_room(i);
                    if (cnt > 0) {
                        char line[64];
                        int L = snprintf(line, sizeof(line),
                                         "  Room %d: %d people\n", i, cnt);
                        safe_send(fd, line, L);
                    }
                }
                safe_send(fd,
                    "Choose the room number or type [new] to create a new room:\n",
                    strlen("Choose the room number or type [new] to create a new room:\n"));

                // get actual choice
                int r2 = recv(fd, req, sizeof(req)-1, 0);
                if (r2 <= 0) { close(fd); free(cli); continue; }
                req[r2] = '\0';
                if (req[r2-1]=='\n') req[r2-1] = '\0';

                if (strcmp(req, "new") == 0) {
                    room_id = allocate_new_room();
                } else {
                    room_id = atoi(req);
                    if (room_id < 1 || room_id > MAX_ROOMS ||
                        count_clients_in_room(room_id) == 0) {
                        safe_send(fd,"Server: invalid room number\n",28);
                        close(fd); free(cli); continue;
                    }
                }
                // confirm
                {
                  char resp[64];
                  snprintf(resp, sizeof(resp),
                           "Connected to %s with room number %d\n",
                           inet_ntoa(cliaddr.sin_addr), room_id);
                  safe_send(fd, resp, strlen(resp));
                }
            }
        } else {
            // user specified “new” or room# immediately
            if (strcmp(req, "new") == 0) {
                room_id = allocate_new_room();
            } else {
                room_id = atoi(req);
            }
            // confirm
            char resp[64];
            snprintf(resp, sizeof(resp),
                     "Connected to %s with room number %d\n",
                     inet_ntoa(cliaddr.sin_addr), room_id);
            safe_send(fd, resp, strlen(resp));
        }

        // finalize and spawn handler
        cli->room_id = room_id;
        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, cli) != 0) {
            close(fd);
            free(cli);
        }
    }

    close(listenfd);
    return 0;
}
