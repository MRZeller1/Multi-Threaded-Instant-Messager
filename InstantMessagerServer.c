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
int clientfds[MAX_CLIENTS];
char clientNames[MAX_CLIENTS][200];
bool active[MAX_CLIENTS];
int usercount = 0;
bool serverRunning = true;
bool chatStarted = false;

// Mutex for managing shared resources
pthread_mutex_t client_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t poll_mutex = PTHREAD_MUTEX_INITIALIZER;

// Poll variables
char pollQuestion[MAXLEN];
char pollAnswers[10][200];
int answersCount = 0;
int pollCount[10];
bool pollActive = false;
time_t pollEndTime;

// Function prototypes
void listUsers(int connfd);
void sendPrivateMessage(int senderIndex, char *message);
void broadcastMessage(int senderIndex, char *message);
void startPoll(int connfd);
void vote(int connfd);
void listCommands(int connfd);
void *pollTimer(void *arg);

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

// Function to handle client communication
void *clientHandler(void *ptr) {
    uint32_t connfd = (uint32_t)ptr;
    int index = -1;
    char buffer[MAXLEN] = {0};
    char name[200] = {0};

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
    write(connfd, "Enter your name: \n", 18);
    ssize_t read_size = read(connfd, name, sizeof(name) - 1);
    if (read_size > 0) {
        name[read_size] = '\0';
        char *newline = strchr(name, '\n');
        if (newline) *newline = '\0';
        
        pthread_mutex_lock(&client_mutex);
        strncpy(clientNames[index], name, sizeof(clientNames[index]) - 1);
        clientNames[index][sizeof(clientNames[index]) - 1] = '\0';
        pthread_mutex_unlock(&client_mutex);

        // Welcome message
        snprintf(buffer, sizeof(buffer), "Welcome to the chat, %s!\n", name);
        write(connfd, buffer, strlen(buffer));
    } else {
        pthread_mutex_lock(&client_mutex);
        active[index] = false;
        clientfds[index] = 0;
        usercount--;
        pthread_mutex_unlock(&client_mutex);
        close(connfd);
        return NULL;
    }

    while (!chatStarted) {
        sleep(1); // Wait for chat to start
    }

    while (serverRunning) {
        memset(buffer, 0, sizeof(buffer));
        write(connfd, "> ", 2);
        
        ssize_t read_size = read(connfd, buffer, sizeof(buffer) - 1);
        if (read_size <= 0) break;
        
        buffer[read_size] = '\0';
        char *newline = strchr(buffer, '\n');
        if (newline) *newline = '\0';

        if (strcmp(buffer, "list") == 0) {
            listUsers(connfd);
        } else if (strncmp(buffer, "send ", 5) == 0) {
            sendPrivateMessage(index, buffer + 5);
        } else if (strncmp(buffer, "broadcast ", 10) == 0) {
            broadcastMessage(index, buffer + 10);
        } else if (strcmp(buffer, "poll") == 0) {
            startPoll(connfd);
        } else if (strcmp(buffer, "vote") == 0) {
            vote(connfd);
        } else if (strcmp(buffer, "close") == 0) {
            break;
        } else if (strcmp(buffer, "commands") == 0) {
            listCommands(connfd);
        } else {
            write(connfd, "Invalid command. Type 'commands' for a list of available commands.\n", 67);
        }
    }

    // Cleanup when client disconnects
    pthread_mutex_lock(&client_mutex);
    active[index] = false;
    close(clientfds[index]);
    clientfds[index] = 0;
    usercount--;
    pthread_mutex_unlock(&client_mutex);
    return NULL;
}

void listUsers(int connfd) {
    char userList[MAXLEN] = "Active users: ";
    pthread_mutex_lock(&client_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (active[i]) {
            strcat(userList, clientNames[i]);
            strcat(userList, " ");
        }
    }
    pthread_mutex_unlock(&client_mutex);
    strcat(userList, "\n");
    write(connfd, userList, strlen(userList));
}

void sendPrivateMessage(int senderIndex, char *message) {
    char senduser[200], sentmessage[MAXLEN];
    sscanf(message, "%s %[^\n]", senduser, message);
    
    pthread_mutex_lock(&client_mutex);
    bool userFound = false;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (active[i] && strcmp(clientNames[i], senduser) == 0) {
            snprintf(sentmessage, sizeof(sentmessage), "%s says: %s\n", clientNames[senderIndex], message);
            write(clientfds[i], sentmessage, strlen(sentmessage));
            write(clientfds[senderIndex], "Message sent!\n", 14);
            userFound = true;
            break;
        }
    }
    pthread_mutex_unlock(&client_mutex);
    
    if (!userFound) {
        snprintf(sentmessage, sizeof(sentmessage), "User %s not found or not active.\n", senduser);
        write(clientfds[senderIndex], sentmessage, strlen(sentmessage));
    }
}

void broadcastMessage(int senderIndex, char *message) {
    char sentmessage[MAXLEN];
    snprintf(sentmessage, sizeof(sentmessage), "%s says: %s\n", clientNames[senderIndex], message);
    
    pthread_mutex_lock(&client_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (active[i] && i != senderIndex) {
            write(clientfds[i], sentmessage, strlen(sentmessage));
        }
    }
    pthread_mutex_unlock(&client_mutex);
    
    write(clientfds[senderIndex], "Broadcast sent!\n", 16);
}

void startPoll(int connfd) {
    pthread_mutex_lock(&poll_mutex);
    if (pollActive) {
        pthread_mutex_unlock(&poll_mutex);
        write(connfd, "A poll is already active.\n", 26);
        return;
    }
    
    write(connfd, "Enter the poll question:\n", 25);
    read(connfd, pollQuestion, sizeof(pollQuestion));
    pollQuestion[strcspn(pollQuestion, "\n")] = 0;
    
    write(connfd, "Enter poll options (up to 10). Type 'done' when finished.\n", 58);
    answersCount = 0;
    char option[200];
    while (answersCount < 10) {
        read(connfd, option, sizeof(option));
        option[strcspn(option, "\n")] = 0;
        if (strcmp(option, "done") == 0) break;
        strcpy(pollAnswers[answersCount], option);
        pollCount[answersCount] = 0;
        answersCount++;
    }
    
    pollActive = true;
    time(&pollEndTime);
    pollEndTime += 90; // Poll lasts 90 seconds
    
    pthread_t timerThread;
    pthread_create(&timerThread, NULL, pollTimer, NULL);
    
    pthread_mutex_unlock(&poll_mutex);
    
    write(connfd, "Poll started!\n", 14);
}

void *pollTimer(void *arg) {
    time_t now;
    do {
        sleep(1);
        time(&now);
    } while (now < pollEndTime && pollActive);
    
    pthread_mutex_lock(&poll_mutex);
    if (pollActive) {
        pollActive = false;
        char result[MAXLEN * 2] = "Poll results:\n";
        strcat(result, pollQuestion);
        strcat(result, "\n");
        for (int i = 0; i < answersCount; i++) {
            char optionResult[MAXLEN];
            snprintf(optionResult, sizeof(optionResult), "%s - %d votes\n", pollAnswers[i], pollCount[i]);
            strcat(result, optionResult);
        }
        
        pthread_mutex_lock(&client_mutex);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (active[i]) {
                write(clientfds[i], result, strlen(result));
            }
        }
        pthread_mutex_unlock(&client_mutex);
    }
    pthread_mutex_unlock(&poll_mutex);
    return NULL;
}

void vote(int connfd) {
    pthread_mutex_lock(&poll_mutex);
    if (!pollActive) {
        pthread_mutex_unlock(&poll_mutex);
        write(connfd, "No active poll.\n", 16);
        return;
    }
    
    char pollInfo[MAXLEN * 2] = "Current poll:\n";
    strcat(pollInfo, pollQuestion);
    strcat(pollInfo, "\n");
    for (int i = 0; i < answersCount; i++) {
        char option[MAXLEN];
        snprintf(option, sizeof(option), "%d. %s\n", i + 1, pollAnswers[i]);
        strcat(pollInfo, option);
    }
    write(connfd, pollInfo, strlen(pollInfo));
    
    write(connfd, "Enter your vote (number):\n", 26);
    char voteStr[10];
    read(connfd, voteStr, sizeof(voteStr));
    int vote = atoi(voteStr) - 1;
    
    if (vote >= 0 && vote < answersCount) {
        pollCount[vote]++;
        write(connfd, "Vote recorded.\n", 15);
    } else {
        write(connfd, "Invalid vote.\n", 14);
    }
    
    pthread_mutex_unlock(&poll_mutex);
}

void listCommands(int connfd) {
    const char *commands = 
        "Available commands:\n"
        "list - List active users\n"
        "send <user> <message> - Send a private message\n"
        "broadcast <message> - Send a message to all users\n"
        "poll - Start a new poll\n"
        "vote - Vote on the current poll\n"
        "close - Quit the chat\n"
        "commands - Show this list\n";
    write(connfd, commands, strlen(commands));
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
        pthread_create(&clientThread, NULL, clientHandler, (void *)(intptr_t)connfd);
    }

    close(listenfd);
    return 0;
}
