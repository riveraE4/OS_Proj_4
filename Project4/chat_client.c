// chat_client.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT    "15000"
#define MAXLEN  512

static int sockfd;
static int got_name;

static void die(const char *msg) {
    perror(msg);
    exit(-1);
}

static void on_sigint(int _){ 
    printf("\nDisconnecting...\n");
    close(sockfd);
    exit(0);
}

static void *reader(void *_) {
    char buf[MAXLEN];
    while (1) {
        ssize_t r = recv(sockfd, buf, sizeof(buf)-1, 0);
        if (r <= 0) die(r<0 ? "recv" : "Server closed");
        buf[r] = 0;
        fputs(buf, stdout);
        if (!got_name && strstr(buf, "Type your user name:")) {
            fputs("Enter your user name: ", stdout);
            fflush(stdout);
        } else if (got_name) {
            fputs("> ", stdout);
            fflush(stdout);
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    struct addrinfo hints={}, *res;
    pthread_t tid;
    char buf[MAXLEN];

    if (argc < 2) {
        fprintf(stderr, "Usage: %s host [new|#]\n", argv[0]);
        exit(1);
    }
    signal(SIGINT, on_sigint);

    // resolve & connect
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(argv[1], PORT, &hints, &res)) die("getaddrinfo");
    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd < 0) die("socket");
    if (connect(sockfd, res->ai_addr, res->ai_addrlen) < 0) die("connect");
    freeaddrinfo(res);

    // room handshake
    if (argc == 2) {
        if (send(sockfd, "\n", 1, 0) < 1) die("send");
        while (1) {
            ssize_t n = recv(sockfd, buf, sizeof(buf)-1, 0);
            if (n <= 0) die("handshake");
            buf[n] = 0;
            fputs(buf, stdout);
            if (strstr(buf, "Choose the room number")) {
                if (!fgets(buf, sizeof(buf), stdin)) die("input");
                buf[strcspn(buf, "\n")] = 0;
                if (send(sockfd, buf, strlen(buf), 0) < 1) die("send");
                break;
            }
            if (strstr(buf, "Connected to")) break;
        }
    } else {
        if (send(sockfd, argv[2], strlen(argv[2]), 0) < 1) die("send");
    }

    puts("Waiting for server...");
    if (pthread_create(&tid, NULL, reader, NULL)) die("pthread");
    pthread_detach(tid);

    // username/chat loop
    while (fgets(buf, sizeof(buf), stdin)) {
        buf[strcspn(buf, "\n")] = 0;
        if (!got_name) got_name = 1;
        if (send(sockfd, buf, strlen(buf), 0) < 1) break;
    }

    close(sockfd);
    return 0;
}
