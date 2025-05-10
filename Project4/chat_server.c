// chat_server.c
// Simple multi-room chat server with auto room creation and menu selection

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT_NUM     15000   // server that is being used to listen on 
#define NAME_LEN     32      // user name length
#define MSG_LEN      512     // message length
#define MAX_CLIENTS  20      // number of  clients
#define MAX_ROOMS    10      // number of chat room 

// per connection stored data
typedef struct {
    int  clisockfd;                 
    char name[NAME_LEN];            
    int  color;                     // ANSI color code
    int  valid;                     
    char ip[INET_ADDRSTRLEN];       
    int  room_id;                   
} ThreadArgs;

static ThreadArgs *clients[MAX_CLIENTS] = {0};
static pthread_mutex_t clients_mtx = PTHREAD_MUTEX_INITIALIZER;

// error and exit
void error(const char *msg) {
    perror(msg);
    exit(1);
}

// number of active clients in room r
int count_clients_in_room(int r) {
    int cnt = 0;
    pthread_mutex_lock(&clients_mtx);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        ThreadArgs *c = clients[i];
        if (c && c->valid && c->room_id == r)
            cnt++;
    }
    pthread_mutex_unlock(&clients_mtx);
    return cnt;
}

// first empty room slot
int allocate_new_room() {
    for (int r = 1; r <= MAX_ROOMS; r++)
        if (count_clients_in_room(r) == 0)
            return r;
    return -1;
}

int safe_send(int fd, const char *msg, size_t len) {
    int n = send(fd, msg, len, MSG_NOSIGNAL);
    if (n < 0)
        fprintf(stderr, "send to %d failed: %s\n", fd, strerror(errno));
    return n;
}

// show connected users 
void print_connected_users() {
    printf("Connected Users:");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        ThreadArgs *c = clients[i];
        if (c && c->valid)
            printf(" %s[%d]", c->name, c->room_id);
    }
    printf("\n");
}

// addition of client to list
void register_client(ThreadArgs *c) {
    pthread_mutex_lock(&clients_mtx);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i]) { clients[i] = c; break; }
    }
    pthread_mutex_unlock(&clients_mtx);
    print_connected_users();
}

// removal of client from list
void deregister_client(ThreadArgs *c) {
    pthread_mutex_lock(&clients_mtx);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] == c) { clients[i] = NULL; break; }
    }
    pthread_mutex_unlock(&clients_mtx);
    print_connected_users();
}

// broadcast a message to everyone in the same room
void broadcast_room(const char *msg, int room) {
    pthread_mutex_lock(&clients_mtx);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        ThreadArgs *c = clients[i];
        if (c && c->valid && c->room_id == room)
            safe_send(c->clisockfd, msg, strlen(msg));
    }
    pthread_mutex_unlock(&clients_mtx);
}

// client handling
void *handle_client(void *arg) {
    ThreadArgs *cli = arg;
    pthread_detach(pthread_self());

    char buf[MSG_LEN];
    int n;
    cli->valid = 1;
    strcpy(cli->name, "Anonymous");

    // Ask for username
    safe_send(cli->clisockfd, "Type your user name: ", 21);
    n = recv(cli->clisockfd, buf, NAME_LEN - 1, 0);
    if (n > 0) {
        buf[n - 1] = '\0';
        strncpy(cli->name, buf, NAME_LEN - 1);

        // Announce join
        cli->color = 31 + rand() % 7;
        snprintf(buf, sizeof(buf),
                 "\033[1;%dm%s joined room %d\033[0m\n",
                 cli->color, cli->name, cli->room_id);
        broadcast_room(buf, cli->room_id);

        // Chat loop
        while ((n = recv(cli->clisockfd, buf, MSG_LEN - 1, 0)) > 0) {
            buf[n] = '\0';
            char out[MSG_LEN + 64];
            snprintf(out, sizeof(out),
                     "\033[1;%dm[%s]\033[0m %s\n",
                     cli->color, cli->name, buf);
            broadcast_room(out, cli->room_id);
        }
    }

    // Announce when a connection leaves
    snprintf(buf, sizeof(buf),
             "\033[1;%dm%s left room %d\033[0m\n",
             cli->color, cli->name, cli->room_id);
    broadcast_room(buf, cli->room_id);

    deregister_client(cli);
    close(cli->clisockfd);
    free(cli);
    return NULL;
}

int main() {
    srand(time(NULL));

    // Create listening socket
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) error("socket");

    // Allow address reuse
    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind to port
    struct sockaddr_in serv = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(PORT_NUM)
    };
    if (bind(listenfd, (struct sockaddr*)&serv, sizeof(serv)) < 0)
        error("bind");

    listen(listenfd, 5);
    printf("Server listening on port %d\n", PORT_NUM);

    while (1) {
        struct sockaddr_in cliaddr;
        socklen_t addrlen = sizeof(cliaddr);
        int fd = accept(listenfd, (struct sockaddr*)&cliaddr, &addrlen);
        if (fd < 0) continue;

        // Initial room request
        char req[16] = {0};
        recv(fd, req, sizeof(req) - 1, 0);
        if (req[strlen(req) - 1] == '\n')
            req[strlen(req) - 1] = '\0';

        // Allocate client(s)
        ThreadArgs *cli = calloc(1, sizeof(*cli));
        cli->clisockfd = fd;
        strncpy(cli->ip, inet_ntoa(cliaddr.sin_addr), INET_ADDRSTRLEN);

        // Room assignment
        if (req[0] == '\0') {
            int any = 0;
            for (int r = 1; r <= MAX_ROOMS; r++)
                if (count_clients_in_room(r) > 0) { any = 1; break; }

            if (!any) {
                cli->room_id = allocate_new_room();
                char msg[64];
                snprintf(msg, sizeof(msg),
                         "Connected to %s with new room %d\n",
                         cli->ip, cli->room_id);
                safe_send(fd, msg, strlen(msg));
            } else {
                safe_send(fd,
                  "Server says following options are available:\n", 44);
                for (int r = 1; r <= MAX_ROOMS; r++) {
                    int cnt = count_clients_in_room(r);
                    if (cnt > 0) {
                        char line[64];
                        int L = snprintf(line, sizeof(line),
                          "  Room %d: %d people\n", r, cnt);
                        safe_send(fd, line, L);
                    }
                }
                safe_send(fd,
                  "Choose the room number or type [new] to create a new room:\n",
                  59);

                int n2 = recv(fd, req, sizeof(req) - 1, 0);
                if (n2 > 0) {
                    req[n2 - 1] = '\0';
                    if (!strcmp(req, "new"))
                        cli->room_id = allocate_new_room();
                    else
                        cli->room_id = atoi(req);
                    char msg[64];
                    snprintf(msg, sizeof(msg),
                             "Connected to %s with room %d\n",
                             cli->ip, cli->room_id);
                    safe_send(fd, msg, strlen(msg));
                } else {
                    close(fd);
                    free(cli);
                    continue;
                }
            }
        } else {
            if (!strcmp(req, "new"))
                cli->room_id = allocate_new_room();
            else
                cli->room_id = atoi(req);

            char msg[64];
            snprintf(msg, sizeof(msg),
                     "Connected to %s with room %d\n",
                     cli->ip, cli->room_id);
            safe_send(fd, msg, strlen(msg));
        }

        register_client(cli);
        pthread_t tid;
        pthread_create(&tid, NULL, handle_client, cli);
        pthread_detach(tid);
    }
    return 0;
}
