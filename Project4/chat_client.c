/* Chris Toala & Eric Rivera */
/* Simple chat client supporting multi-room selection and username handshake */

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

#define PORT_NUM  "15000"  /* server port number */
#define MAXLEN    512      /* max messages */

static int sockfd;                         /* socket descriptor */

static int got_name;

/* error and exit */
static void error(const char *msg) {
    perror(msg);
    exit(1);
}

/* AI Influence: Added SIGINT handler to gracefully disconnect on Ctrl-C */
static void on_sigint(int sig) {
    (void)sig;  /* unused parameter */
    printf("\nDisconnecting...\n");
    close(sockfd);
    exit(0);
}

/* reading of messages and displaying of messages */
static void *reader(void *arg) {
    (void)arg;
    char buf[MAXLEN];
    while (1) {
        ssize_t r = recv(sockfd, buf, sizeof(buf)-1, 0);
        if (r <= 0) {
            /* error or server closed connection */
            if (r < 0)
                error("recv");
            else
                exit(-1);
        }
        buf[r] = '\0';            /* null termination */
        fputs(buf, stdout);       /* print server message */

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
    struct addrinfo hints = {0}, *res;
    pthread_t tid;
    char buf[MAXLEN];

    if (argc < 2) {
        fprintf(stderr, "Usage: %s host [new|room#]\n", argv[0]);
        exit(1);
    }

    /* AI Influence: Register Ctrl-C handler for graceful exit */
    signal(SIGINT, on_sigint);

    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(argv[1], PORT_NUM, &hints, &res) != 0)
        error("getaddrinfo");

    /* Create socket */
    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd < 0)
        error("socket");

    /* Connect to server */
    if (connect(sockfd, res->ai_addr, res->ai_addrlen) < 0)
        error("connect");
    freeaddrinfo(res);

    /* If no room argument, trigger the server’s menu */
    if (argc == 2) {
        /* send blank line to trigger menu */
        if (send(sockfd, "\n", 1, 0) < 1)
            error("send");

        /* read server responses until we see “Choose the room number” or “Connected to…” */
        while (1) {
            ssize_t n = recv(sockfd, buf, sizeof(buf)-1, 0);
            if (n <= 0) {
                if (n < 0) error("handshake recv");
                else       exit(-1);
            }
            buf[n] = '\0';
            fputs(buf, stdout);

            if (strstr(buf, "Choose the room number")) {
                if (!fgets(buf, sizeof(buf), stdin)) exit(-1);
                buf[strcspn(buf, "\n")] = '\0';
                if (send(sockfd, buf, strlen(buf), 0) < 1)
                    error("send");
                break;
            }
            if (strstr(buf, "Connected to"))
                break;
        }

    } else {
        if (send(sockfd, argv[2], strlen(argv[2]), 0) < 1)
            error("send");
    }

    puts("Waiting for server...");

    /* Launch reader thread */
    if (pthread_create(&tid, NULL, reader, NULL) != 0)
        error("pthread_create");
    pthread_detach(tid);

    /* Main loop: send username then chat messages */
    while (fgets(buf, sizeof(buf), stdin)) {
        buf[strcspn(buf, "\n")] = '\0';

        if (!got_name) got_name = 1;

        if (send(sockfd, buf, strlen(buf), 0) < 1)
            break;  /* server closed or error */
    }

    close(sockfd);
    return 0;
}
