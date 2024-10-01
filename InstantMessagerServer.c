#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include <sys/types.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>

#define MAXWORDS 100000
#define MAXLEN 1000
#define MAX_CLIENTS 1024

void pexit(char *errmsg) {
    fprintf(stderr, "%s\n", errmsg);
    exit(1);
}

// Global variables for server and client management
char *words[MAXWORDS];
int clientfds[MAX_CLIENTS];
char clientNames[MAX_CLIENTS][200];
bool active[MAX_CLIENTS];
int numWords = 0;
bool serverRunning = true;
bool chatStarted = false;
int usercount = 0;

// Mutex for managing shared resources
pthread_mutex_t client_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t poll_mutex = PTHREAD_MUTEX_INITIALIZER;

// Poll variables
char pollQuestion[MAXLEN];
char pollAnswers[10][200];
int answersCount = 0;
int pollCount[10];
bool pollActive = false;

// Signal handler to shut down the server
void sighandler(int signum) {
    printf("Received SIGINT signal. Shutting down the server.\n");
    serverRunning = false;
    pthread_mutex_lock(&client_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clientfds[i] != 0) {
            close(clientfds[i]);
            clientfds[i] = 0;
        }
    }
    pthread_mutex_unlock(&client_mutex);
}

// Poll handler to process and announce poll results
void pollHandler(int signo) {
    pthread_mutex_lock(&poll_mutex);
    pollActive = false;
    char result[MAXLEN] = "The results are in!\n";
    strcat(result, pollQuestion);
    strcat(result, "\n");
    for (int i = 0; i < answersCount; i++) {
        char numberString[10];
        sprintf(numberString, " - %d votes\n", pollCount[i]);
        strcat(result, pollAnswers[i]);
        strcat(result, numberString);
    }
    pthread_mutex_unlock(&poll_mutex);

    pthread_mutex_lock(&client_mutex);
    for (int i = 0; i < usercount; i++) {
        if (active[i]) {
            write(clientfds[i], result, strlen(result));
        }
    }
    pthread_mutex_unlock(&client_mutex);
}

// Function to handle client communication
void *clientHandler(void *ptr) {
    uint32_t connfd = (uint32_t)ptr;
    int index = -1;

    pthread_mutex_lock(&client_mutex);
    // Assign client to an available slot
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clientfds[i] == 0) {
            clientfds[i] = connfd;
            index = i;
            break;
        }
    }

    // If server is full
    if (index == -1) {
        pthread_mutex_unlock(&client_mutex);
        close(connfd);
        fprintf(stderr, "Too many clients.\n");
        return NULL;
    }

    usercount++;
    active[index] = true;
    pthread_mutex_unlock(&client_mutex);

    // Prompt client for their name
    char *prompt = "Enter your name: \n";
    write(connfd, prompt, strlen(prompt));
    read(connfd, clientNames[index], 200);
    char *cptr = strchr(clientNames[index], '\n');
    if (cptr) *cptr = '\0';

    // Welcome message
    char buffer[1024];
    sprintf(buffer, "Welcome to the chat, %s!\n", clientNames[index]);
    write(connfd, buffer, strlen(buffer));

    while (!chatStarted) {
        sleep(1); // Wait for chat to start
    }

    bool quit = false;
    while (!quit && serverRunning) {
        char command[MAXLEN];
        char senduser[MAXLEN];
        char message[MAXLEN];
        char sentmessage[MAXLEN];
        char userList[MAXLEN] = "";

        // Prompt for command
        sprintf(buffer, ">\n");
        write(connfd, buffer, strlen(buffer));

        // Read client command
        if (read(connfd, buffer, sizeof(buffer)) <= 0) break;

        sscanf(buffer, "%s", command);

        // Handle 'list' command: List active users
        if (strcmp(command, "list") == 0) {
            pthread_mutex_lock(&client_mutex);
            for (int i = 0; i < usercount; i++) {
                if (active[i]) {
                    strcat(userList, clientNames[i]);
                    strcat(userList, " ");
                }
            }
            pthread_mutex_unlock(&client_mutex);
            write(connfd, userList, strlen(userList));
            write(connfd, "\n", 1);

        // Handle 'send' command: Send private message
        } else if (strcmp(command, "send") == 0 && sscanf(buffer, "%s %s %[^\n]", command, senduser, message) == 3) {
            bool userFound = false;
            pthread_mutex_lock(&client_mutex);
            for (int i = 0; i < usercount; i++) {
                if (strcmp(clientNames[i], senduser) == 0) {
                    if (active[i]) {
                        sprintf(sentmessage, "%s says: %s\n", clientNames[index], message);
                        write(clientfds[i], sentmessage, strlen(sentmessage));
                        write(connfd, "Message sent!\n", 14);
                    } else {
                        char sorryMessage[MAXLEN] = "Sorry, ";
                        strcat(sorryMessage, senduser);
                        strcat(sorryMessage, " is not active.\n");
                        write(connfd, sorryMessage, strlen(sorryMessage));
                    }
                    userFound = true;
                    break;
                }
            }
            pthread_mutex_unlock(&client_mutex);
            if (!userFound) {
                char sorryMessage[MAXLEN] = "Sorry, ";
                strcat(sorryMessage, senduser);
                strcat(sorryMessage, " is not a valid user.\n");
                write(connfd, sorryMessage, strlen(sorryMessage));
            }

        // Handle 'broadcast' command: Broadcast message to all users
        } else if (strcmp(command, "broadcast") == 0 && sscanf(buffer, "%s %[^\n]", command, message) == 2) {
            pthread_mutex_lock(&client_mutex);
            if (active[index]) {
                sprintf(sentmessage, "%s says: %s\n", clientNames[index], message);
                for (int i = 0; i < usercount; i++) {
                    if (active[i] && i != index) {
                        write(clientfds[i], sentmessage, strlen(sentmessage));
                    }
                }
                write(connfd, "Broadcast sent!\n", 16);
            } else {
                write(connfd, "You are not active.\n", 20);
            }
            pthread_mutex_unlock(&client_mutex);

        // Handle 'close' command: Quit the chat
        } else if (strcmp(command, "close") == 0) {
            quit = true;

        // Handle 'poll' command: Start a poll
        } else if (strcmp(command, "poll") == 0) {
            pthread_mutex_lock(&poll_mutex);
            if (pollActive) {
                pthread_mutex_unlock(&poll_mutex);
                write(connfd, "Poll is currently active.\n", 26);
                continue;
            }
            write(connfd, "What is the question you want to ask?\n", 39);
            read(connfd, pollQuestion, sizeof(pollQuestion));
            write(connfd, "Write up to 10 answers, one per line. Type 'DONE' when finished.\n", 64);
            answersCount = 0;
            while (answersCount < 10) {
                read(connfd, buffer, sizeof(buffer));
                if (strstr(buffer, "DONE") != NULL) {
                    break;
                }
                strcpy(pollAnswers[answersCount], buffer);
                pollCount[answersCount] = 0;
                answersCount++;
            }
            srand(time(NULL) + getpid() + getuid());
            signal(SIGALRM, pollHandler);
            alarm(90); // Poll lasts 90 seconds
            pollActive = true;
            pthread_mutex_unlock(&poll_mutex);

        // Handle 'vote' command: Vote on the current poll
        } else if (strcmp(command, "vote") == 0) {
            int voteIndex;
            pthread_mutex_lock(&poll_mutex);
            if (!pollActive) {
                pthread_mutex_unlock(&poll_mutex);
                write(connfd, "No active polls.\n", 17);
                continue;
            }
            write(connfd, "Enter your vote (number):\n", 26);
            read(connfd, buffer, sizeof(buffer));
            sscanf(buffer, "%d", &voteIndex);
            if (voteIndex < 1 || voteIndex > answersCount) {
                pthread_mutex_unlock(&poll_mutex);
                write(connfd, "Invalid vote.\n", 14);
                continue;
            }
            pollCount[voteIndex - 1]++;
            pthread_mutex_unlock(&poll_mutex);
            write(connfd, "Vote recorded.\n", 15);

        // Handle 'commands' command: List available commands
        } else if (strcmp(command, "commands") == 0) {
            write(connfd, "Available commands:\n", 20);
            write(connfd, "list - List active users\n", 26);
            write(connfd, "send <user> <message> - Send a private message\n", 46);
            write(connfd, "broadcast <message> - Broadcast a message to all users\n", 55);
            write(connfd, "poll - Create a new poll\n", 26);
            write(connfd, "vote <option> - Vote on the current poll\n", 41);
            write(connfd, "close - Quit the chat\n", 23);
        } else {
            write(connfd, "Invalid command. Type 'commands' for a list of available commands.\n", 67);
        }
    }

    // Cleanup when client disconnects
    pthread_mutex_lock(&client_mutex);
    active[index] = false;
    clientfds[index] = 0;
    usercount--;
    pthread_mutex_unlock(&client_mutex);
    close(connfd);
    return NULL;
}

// Function to start chat session
void *startChat(void *ptr) {
    chatStarted = true;
    printf("Server chat started.\n");
    return NULL;
}

int main() {
    int listenfd = 0;
    struct sockaddr_in serv_addr;
    pthread_t managerThread;

    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        pexit("socket() error.");

    memset(&serv_addr, '0', sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Bind to an available port
    int port = 4999;
    do {
        port++;
        serv_addr.sin_port = htons(port);
    } while (bind(listenfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0);
    printf("bind() succeeds for port #%d\n", port);

    if (listen(listenfd, 10) < 0)
        pexit("listen() error.");

    pthread_create(&managerThread, NULL, startChat, NULL);

    signal(SIGINT, sighandler);

    while (serverRunning) {
        int connfd = accept(listenfd, (struct sockaddr *)NULL, NULL);
        if (connfd < 0)
            continue;

        pthread_t clientThread;
        pthread_create(&clientThread, NULL, clientHandler, (void *)connfd);
    }

    close(listenfd);
    return 0;
}
