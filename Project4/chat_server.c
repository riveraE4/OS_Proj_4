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
#include <fcntl.h>

#define PORT_NUM 1004
#define NAME_LEN 32
#define MAX_CLIENTS 10
#define MSG_LEN 512

void error(const char *msg)
{
    perror(msg);
    exit(1);
}

typedef struct _ThreadArgs {
    int clisockfd; 
    char name[NAME_LEN]; // storing of username
    int color; // Ansi color code
    int valid; // Flag to track if socket is valid
} ThreadArgs;

//client list and mutex
static ThreadArgs *clients[MAX_CLIENTS] = {0};
static pthread_mutex_t clients_mtx = PTHREAD_MUTEX_INITIALIZER;

// Ignore SIGPIPE signals to prevent program termination
void setup_signal_handler() {
    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGPIPE, &sa, NULL);
}

// Function to safely send data to a socket
int safe_send(int sockfd, const char *msg, size_t len) {
    if (sockfd < 0) return -1;
    
    // Try to send with NOSIGNAL to prevent SIGPIPE
    int ret = send(sockfd, msg, len, MSG_NOSIGNAL);
    if (ret < 0) {
        printf("Send error: %s (errno=%d)\n", strerror(errno), errno);
    }
    return ret;
}

// Print connected users (for server logs)
void print_connected_users() 
{
    printf("Connected Users: ");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && clients[i]->valid) printf(" %s", clients[i]->name);
    }
    printf("\n");
}

void broadcast(const char *msg) //broadcasting a message to all clients
{
    pthread_mutex_lock(&clients_mtx);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i] && clients[i]->valid) {
            // Try to send, if it fails, mark this client for removal
            if (safe_send(clients[i]->clisockfd, msg, strlen(msg)) < 0) {
                printf("Failed to send to client %s\n", clients[i]->name);
                clients[i]->valid = 0;
            }
        }
    }
    
    // Clean up invalid clients
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i] && !clients[i]->valid) {
            printf("Cleaning up invalid client: %s\n", clients[i]->name);
            close(clients[i]->clisockfd);
            free(clients[i]);
            clients[i] = NULL;
        }
    }
    pthread_mutex_unlock(&clients_mtx);
}

void register_client(ThreadArgs *c)
{
    pthread_mutex_lock(&clients_mtx);
    int added = 0;
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (!clients[i]) {
            clients[i] = c;
            added = 1;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mtx);
    
    if (!added) {
        printf("WARNING: Maximum clients reached, couldn't add %s\n", c->name);
        // Send a message to the client and close connection
        const char *msg = "Server is full, try again later.\n";
        safe_send(c->clisockfd, msg, strlen(msg));
        close(c->clisockfd);
        free(c);
        return;
    }
    
    print_connected_users();
}

void deregister_client(ThreadArgs *c)
{
    pthread_mutex_lock(&clients_mtx);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i] == c) {
            clients[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mtx);
    print_connected_users();
}

void *handle_client(void *arg)
{
    pthread_detach(pthread_self());
    ThreadArgs *cli = (ThreadArgs *)arg;
    char buf[MSG_LEN];
    int n;

    // Initialize client
    cli->valid = 1;
    memset(cli->name, 0, NAME_LEN);
    strcpy(cli->name, "Anonymous"); // Default name in case user disconnects early

    // Send welcome message and request username
    const char *ask = "Type username: ";
    if (safe_send(cli->clisockfd, ask, strlen(ask)) < 0) {
        printf("Error sending username prompt\n");
        close(cli->clisockfd);
        free(cli);
        return NULL;
    }
    
    // Wait for username
    n = recv(cli->clisockfd, buf, NAME_LEN - 1, 0);
    if (n <= 0) {
        printf("Client disconnected during username entry\n");
        close(cli->clisockfd);
        free(cli);
        return NULL;
    }
    
    // Process username
    buf[n] = '\0';
    if (strlen(buf) > 0) {
        // Remove any trailing newline
        if (buf[strlen(buf)-1] == '\n') {
            buf[strlen(buf)-1] = '\0';
        }
        strncpy(cli->name, buf, NAME_LEN - 1);
        cli->name[NAME_LEN - 1] = '\0'; // Ensure null termination
    }
    
    // Assign color and register
    cli->color = 31 + (rand() % 7);
    register_client(cli);
    
    // Create welcome message with a newline at the end
    snprintf(buf, sizeof(buf), "\033[1;%dm%s joined the chat!\033[0m\n", cli->color, cli->name);
    broadcast(buf);

    // Main message processing loop
    while (1) {
        n = recv(cli->clisockfd, buf, MSG_LEN - 1, 0);
        if (n <= 0) {
            if (n < 0) {
                printf("Error receiving from %s: %s\n", cli->name, strerror(errno));
            } else {
                printf("Client %s disconnected\n", cli->name);
            }
            break;
        }
        
        buf[n] = '\0';
        
        // Add newline if not present
        char out[MSG_LEN + NAME_LEN + 32];
        if (buf[strlen(buf)-1] != '\n') {
            snprintf(out, sizeof(out), "\033[1;%dm[%s]\033[0m: %s\n", cli->color, cli->name, buf);
        } else {
            snprintf(out, sizeof(out), "\033[1;%dm[%s]\033[0m: %s", cli->color, cli->name, buf);
        }
        
        broadcast(out);
    }
    
    // Announce departure
    snprintf(buf, sizeof(buf), "\033[1;%dm%s left the chat!\033[0m\n", cli->color, cli->name);
    broadcast(buf);
    
    // Clean up this client
    deregister_client(cli);
    close(cli->clisockfd);
    free(cli);
    return NULL;
}

int main(int argc, char *argv[])
{
    // Set up signal handler
    setup_signal_handler();
    
    srand(time(NULL));
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) error("ERROR opening socket");
    
    // Set socket option to reuse address
    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        error("ERROR setting socket options");
    }

    struct sockaddr_in serv_addr;
    socklen_t slen = sizeof(serv_addr);
    memset((char*) &serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;    
    serv_addr.sin_port = htons(PORT_NUM);

    int status = bind(sockfd, 
            (struct sockaddr*) &serv_addr, slen);
    if (status < 0) error("ERROR on binding");

    listen(sockfd, 5);
    printf("Chat server started on port %d\n", PORT_NUM);

    while(1) {
        struct sockaddr_in cli_addr;
        socklen_t clen = sizeof(cli_addr);
        int newsockfd = accept(sockfd, 
            (struct sockaddr *) &cli_addr, &clen);
        if (newsockfd < 0) error("ERROR on accept");

        printf("Connected: %s\n", inet_ntoa(cli_addr.sin_addr));

        ThreadArgs *cli = malloc(sizeof(ThreadArgs));
        if (!cli) error("malloc");
        cli->clisockfd = newsockfd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, cli)!=0) {
            perror("pthread_create");
            close(newsockfd);
            free(cli);
        }
    }

    close(sockfd);
    return 0; 
}