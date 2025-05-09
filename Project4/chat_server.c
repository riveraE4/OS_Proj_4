#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>


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
} ThreadArgs;

//client list and mutex
static ThreadArgs *clients[MAX_CLIENTS] = {0};
static pthread_mutex_t clients_mtx = PTHREAD_MUTEX_INITIALIZER;

void broadcast(const char *msg) //broadcasting a message to all clients
{
	pthread_mutex_lock(&clients_mtx);
	for (int i = 0; i < MAX_CLIENTS; ++i) {
		if (clients[i]) {
			send(clients[i]->clisockfd, msg, strlen(msg), 0);
		}
	}
	pthread_mutex_unlock(&clients_mtx);
}

void register_client(ThreadArgs *c)
{
	pthread_mutex_lock(&clients_mtx);
	for (int i = 0; i < MAX_CLIENTS; ++i) {
		if (!clients[i]) {
			clients[i] = c;
			break;
		}
	}
	pthread_mutex_unlock(&clients_mtx);
	printf("Connected Users: ");
	for ( int i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i]) printf(" %s", clients[i]->name);
	}
	printf("\n");
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
	printf("Connected Users: ");
	for ( int i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i]) printf(" %s", clients[i]->name);
	}
	printf("\n");
}

void *handle_client(void *arg)
{
	pthread_detach(pthread_self());
	ThreadArgs *cli = (ThreadArgs *)arg;
	char buf[MSG_LEN];
	int n;

	// recieve usernames
	const char *ask = "Type username: ";
	send(cli->clisockfd, ask, strlen(ask), 0);
	n = recv(cli->clisockfd, buf, NAME_LEN - 1, 0);
	if (n <= 0) {
		close(cli->clisockfd);
		free(cli);
		return NULL;
	}
	buf[n] = '\0';
	strncpy(cli->name, buf, NAME_LEN);

	//color assign
	cli->color = 31 + (rand() % 7);

	//broadcast join
	register_client(cli);
	snprintf(buf, sizeof(buf), "\033[1;%dm%s joined the chat!\n\033[0m", cli->color, cli->name);
	broadcast(buf);

	while ((n = recv(cli->clisockfd, buf, MSG_LEN - 1, 0)) > 0) {
		buf[n] = '\0';
		char out[MSG_LEN + NAME_LEN + 16];
		snprintf(out, sizeof(out), "\033[1;%dm[%s]\033[0m: %s", cli->color, cli->name, buf);
		broadcast(out);
	}
	snprintf(buf, sizeof(buf), "\033[1;%dm%s left the chat!\n\033[0m", cli->color, cli->name);
	broadcast(buf);
	deregister_client(cli);

	close(cli->clisockfd);
	free(cli);
	return NULL;


}

// void* thread_main(void* args)
// {
// 	// make sure thread resources are deallocated upon return
// 	pthread_detach(pthread_self());

// 	// get socket descriptor from argument
// 	int clisockfd = ((ThreadArgs*) args)->clisockfd;
// 	free(args);

// 	//-------------------------------
// 	// Now, we receive/send messages
// 	char buffer[256];
// 	int nsen, nrcv;

// 	nrcv = recv(clisockfd, buffer, 256, 0);
// 	if (nrcv < 0) error("ERROR recv() failed");

// 	while (nrcv > 0) {
// 		nsen = send(clisockfd, buffer, nrcv, 0);
// 		if (nsen != nrcv) error("ERROR send() failed");

// 		nrcv = recv(clisockfd, buffer, 256, 0);
// 		if (nrcv < 0) error("ERROR recv() failed");
// 	}

// 	close(clisockfd);
// 	//-------------------------------

// 	return NULL;
// }

int main(int argc, char *argv[])
{
	srand(time(NULL));
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) error("ERROR opening socket");

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

	while(1) {
		struct sockaddr_in cli_addr;
		socklen_t clen = sizeof(cli_addr);
		int newsockfd = accept(sockfd, 
			(struct sockaddr *) &cli_addr, &clen);
		if (newsockfd < 0) error("ERROR on accept");

		printf("Connected: %s\n", inet_ntoa(cli_addr.sin_addr));

		ThreadArgs *cli = malloc(sizeof(ThreadArgs));
		if (!cli) error ("malloc");
		cli->clisockfd = newsockfd;

		pthread_t tid;
		if (pthread_create(&tid, NULL, handle_client, cli)!=0)
		{
			perror("pthread_create");
			close(newsockfd);
			free(cli);
		}

		// prepare ThreadArgs structure to pass client socket
		// ThreadArgs* args = (ThreadArgs*) malloc(sizeof(ThreadArgs));
		// if (args == NULL) error("ERROR creating thread argument");
		
		// args->clisockfd = newsockfd;

		// pthread_t tid;
		// if (pthread_create(&tid, NULL, thread_main, (void*) args) != 0) error("ERROR creating a new thread");
	}

	close(sockfd);
	return 0; 
}

