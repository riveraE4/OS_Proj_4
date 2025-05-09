#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>

#define PORT_NUM 1004
#define NAME_LEN 32
#define MSG_LEN 512

int sockfd; // Global socket descriptor for cleanup in signal handler
int username_provided = 0; // Flag to track if username has been set

void error(const char *msg)
{
    perror(msg);
    exit(0);
}

// Signal handler for Ctrl+C
void handle_signal(int sig) 
{
    printf("\nDisconnecting from server...\n");
    close(sockfd);
    exit(0);
}

// Thread function to continuously receive messages from server
void* receive_messages(void* arg) 
{
    char buffer[MSG_LEN];
    int n;
    
    while (1) {
        memset(buffer, 0, sizeof(buffer));
        n = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
        
        if (n <= 0) {
            if (n < 0) {
                fprintf(stderr, "Error receiving message: %s\n", strerror(errno));
            } else {
                printf("Server disconnected\n");
            }
            close(sockfd);
            exit(1);
        }
        
        buffer[n] = '\0';
        printf("%s", buffer); // Print the received message
        
        // Check if this is the username prompt
        if (!username_provided && strstr(buffer, "Type username:") != NULL) {
            printf("Enter your username: ");
            fflush(stdout);
        } else {
            // For regular messages, add a prompt for the next message
            if (username_provided) {
                printf("> ");
                fflush(stdout);
            }
        }
    }
    
    return NULL;
}

int main(int argc, char *argv[])
{
    // Set up signal handler for graceful exit
    signal(SIGINT, handle_signal);
    
    if (argc < 2) {
        fprintf(stderr, "Usage: %s hostname\n", argv[0]);
        exit(1);
    }
    
    printf("Try connecting to %s...\n", argv[1]);
    
    // Create socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) error("ERROR opening socket");
    
    // Get server information
    struct hostent* server = gethostbyname(argv[1]);
    if (server == NULL) error("ERROR, no such host");
    
    // Set up server address
    struct sockaddr_in serv_addr;
    socklen_t slen = sizeof(serv_addr);
    memset((char*) &serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    memcpy((char*)&serv_addr.sin_addr.s_addr,
           (char*)server->h_addr,
           server->h_length);
    serv_addr.sin_port = htons(PORT_NUM);
    
    // Connect to server
    int status = connect(sockfd, (struct sockaddr *) &serv_addr, slen);
    if (status < 0) error("ERROR connecting");
    
    printf("Connected to server. Waiting for welcome message...\n");
    
    // Start thread to receive messages
    pthread_t recv_thread;
    if (pthread_create(&recv_thread, NULL, receive_messages, NULL) != 0) {
        error("ERROR creating receiver thread");
    }
    
    // Main loop for sending messages
    char buffer[MSG_LEN];
    int n;
    
    while (1) {
        memset(buffer, 0, sizeof(buffer));
        
        // Read input from user
        if (!fgets(buffer, sizeof(buffer) - 1, stdin)) {
            break; // Exit on EOF
        }
        
        // Remove trailing newline if present
        size_t len = strlen(buffer);
        if (len > 0 && buffer[len-1] == '\n') {
            buffer[len-1] = '\0';
            len--;
        }
        
        // Track when username is provided
        if (!username_provided) {
            username_provided = 1;
        }
        
        // Send message to server
        n = send(sockfd, buffer, strlen(buffer), 0);
        if (n < 0) {
            fprintf(stderr, "ERROR sending message: %s\n", strerror(errno));
            break;
        }
    }
    
    // Clean up
    close(sockfd);
    return 0;
}