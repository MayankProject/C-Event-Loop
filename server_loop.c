#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sys/poll.h>
#include <stdbool.h>
#include <netinet/in.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#define PORT 8090
#define MAXLEN 1024

typedef struct Conn{
    int fd;
    char incoming[MAXLEN];
    char outgoing[MAXLEN+100];
    bool want_read; 
    bool want_write;
    bool want_close;
} Conn;

typedef struct serverState{
    Conn** fd2Conn;
    int home_fd;
    struct pollfd* fds_polling_arg;
    int pollArgsLen;
    int maxFd;
    int fds;
} serverState;

serverState State;

void init(){
    State.fds = 0;
    State.home_fd = -1;
    State.fd2Conn = NULL;
    State.fds_polling_arg = NULL;
}

void die(const char *msg){
    perror(msg);
    exit(1);
}

static void fd_set_nb(int fd) {
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (errno) {
        die("fcntl error");
        return;
    }

    flags |= O_NONBLOCK;

    errno = 0;
    (void)fcntl(fd, F_SETFL, flags);
    if (errno) {
        die("fcntl error");
    }
}


void handle_accept(int fd){
    struct sockaddr_storage client_addr;
    socklen_t len = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &len);
    printf("connection on fd: %i\n", connfd);
    if (connfd < 0) {
        die("accept() error");
        return;
    }
    fd_set_nb(connfd);
    Conn *newConn = malloc(sizeof(Conn));
    newConn->fd = connfd;
    newConn->want_read = true;
    newConn->want_write = false;
    newConn->want_close = false;
    if(connfd > State.maxFd){
        State.maxFd = connfd;
        State.fds++;
        State.fd2Conn = realloc(State.fd2Conn, sizeof(Conn*)*(State.maxFd+1));
    }
    else if(State.fd2Conn[connfd] != NULL){
        free(State.fd2Conn[connfd]);
    }
    State.fd2Conn[connfd] = newConn;
}

void process_request(Conn *conn){
    snprintf(conn->outgoing, sizeof(conn->outgoing),  "Hello this is from server, You sent \"%s\"?", conn->incoming);
    conn->outgoing[strlen(conn->outgoing)] = '\0';
    conn->want_read = false;
    conn->want_write = true;
    conn->incoming[0] = '\0';
}

void handle_read(Conn *conn){
    ssize_t rv = read(conn->fd, conn->incoming, sizeof(conn->incoming)-1);
    if (rv < 0) {
        if (errno == EAGAIN) {
            return; // actually not ready
        }
        die("read() error");
    }
    if (rv == 0) {
        printf("client closed %i \n", conn->fd);
        conn->want_close = true;
        return; 
    }
    conn->incoming[rv] = '\0';
    process_request(conn);
}

void handle_write(Conn *conn){
    ssize_t rv = write(conn->fd, conn->outgoing, strlen(conn->outgoing));
    if (rv < 0) {
        if (errno == EAGAIN) {
            return; // actually not ready
        }
        die("write() error");
    }
    if (rv == 0) {
        conn->want_close = true;
        return;
    }
    conn->outgoing[rv] = '\0';
    conn->want_read = true; 
    conn->want_write = false;
}

int main(){
    init();
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket()");
    }
    printf("home_socket: %i\n", fd);
    fd_set_nb(fd);

    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
        perror("setsockopt");
        exit(1);
    }
    int ret = bind(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0) {
        die("bind()");
    }
    ret = listen(fd, SOMAXCONN);
    if (ret < 0) {
        die("listen()");
    }
    State.home_fd = fd;
    State.maxFd = fd;
    State.fd2Conn = calloc((State.maxFd+1), sizeof(Conn*));
    while(true){
        if(State.fds_polling_arg == NULL){
            State.fds_polling_arg = malloc(sizeof(struct pollfd));
        }
        int new_size = State.fds + 1;
        if(State.pollArgsLen <= new_size){
            State.fds_polling_arg = realloc(State.fds_polling_arg, sizeof(struct pollfd)*(new_size));
        }

        struct pollfd newPollArg;
        newPollArg.fd = fd;
        newPollArg.events = POLLERR | POLLIN;
        State.fds_polling_arg[0] = newPollArg;
        State.pollArgsLen = 1;

        for (int i = 0; i < State.maxFd+1; i++) {
            Conn *conn = State.fd2Conn[i];
            if(!conn){
                continue;
            }
            if(conn->want_close){
                continue;
            }
            struct pollfd newPollArg;
            newPollArg.fd = conn->fd;
            newPollArg.events = POLLERR;
            if(conn->want_read){
                newPollArg.events = newPollArg.events | POLLIN;
            }
            if(conn->want_write){
                newPollArg.events = newPollArg.events | POLLOUT;
            }
            State.fds_polling_arg[State.pollArgsLen] = newPollArg;
            State.pollArgsLen++;
        }

        int rv = poll(State.fds_polling_arg, State.pollArgsLen, -1);
        if (rv < 0 ) {
            if (errno == EINTR) {
                continue;   // sys call interrupted by signal
            }
            die("poll");
        }
        struct pollfd home_socket_arg = State.fds_polling_arg[0];
        if (home_socket_arg.revents) {
            handle_accept(home_socket_arg.fd);
        }

        for (int i = 1; i < State.pollArgsLen; i++) {
            struct pollfd listening_socket = State.fds_polling_arg[i];
            Conn *conn = State.fd2Conn[listening_socket.fd];
            uint8_t ready = listening_socket.revents;
            if (conn->want_close){
                State.fds--;
                close(conn->fd);
                State.fd2Conn[conn->fd] = NULL;
                free(conn);
                continue;
            }
            if (ready & POLLIN) {
                handle_read(conn);
            }
            if (ready & POLLOUT) {
                handle_write(conn);
            }
        }
    }
}
