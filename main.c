#include <stdio.h>
#include <sys/resource.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/wait.h>

#define MAX_CONN 1024
#define MAX_BACK_LOG 256
#define PORT 6000

#define SOCK_SERV_SUCCESS 0
#define SOCK_SERV_ERROR   -1
#define WORKERS 2

int fork_worker(int listen_fd, int epoll_fd) {
    int conn_fd;

    struct sockaddr_in client_addr;
    struct epoll_event ev;
    struct epoll_event evs[MAX_CONN];

    int cur_fds;
    cur_fds = 1;

    socklen_t len = sizeof(struct sockaddr_in);
    while (1) {
        int wait_fds;
        if ((wait_fds = epoll_wait(epoll_fd, evs, cur_fds, -1)) == -1) {
            printf("[%d], Epoll wait falure : %d, epoll_fd : %d, cur_fds : %d\n", getpid(), errno, epoll_fd, cur_fds);

            continue;
        }

        for (int i = 0; i < wait_fds; i++) {
            printf("[%d], Epoll wait awake, epoll_fd : %d , conn fd : %d, cur_fds : %d\n", getpid(), epoll_fd, evs[i].data.fd, cur_fds);

            if (evs[i].data.fd == listen_fd && cur_fds < MAX_CONN) {

                if( ( conn_fd = accept( listen_fd, (struct sockaddr *)&client_addr, &len ) ) == -1 ) {
                    printf("[%d], Failed to accept conn: %d \n", getpid(), errno);

                    return SOCK_SERV_ERROR;
                }

                printf("[%d], Accept from IP : %s, port : %d\n", getpid(), inet_ntoa(client_addr.sin_addr),
                       ntohs(client_addr.sin_port));

                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = conn_fd;

                printf("[%d], Add conn fd to epoll, epoll fd : %d, conn fd : %d\n",  getpid(), epoll_fd, conn_fd);
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev) < 0) {
                    printf("[%d], Failed to add epoll conn fd : %d\n", getpid(), errno);
                    return SOCK_SERV_ERROR;
                }

                ++cur_fds;
                
                continue;
            }


            int nread;
            char buf[1024];
            nread = read(evs[i].data.fd, &buf, sizeof(buf));

            if (nread <= 0) {
                close(evs[i].data.fd);

                printf("[%d], Delete conn fd from epoll, epoll_fd : %d, conn_fd : %d\n", getpid(), epoll_fd, evs[i].data.fd);
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, evs[i].data.fd, &ev);
                --cur_fds;

                continue;
            }

            printf("[%d], received: [%s] \n", getpid(), buf);
            write(evs[i].data.fd, buf, nread);
        }
    }

    return SOCK_SERV_SUCCESS;
}

int listen_sock(int *sock_fd, int *epoll_fd, struct sockaddr_in *server_addr) {
    int listen_fd;
    struct epoll_event ev;

    int reuseport = 1;

    //crete socket fd
    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        printf("[%d], Create socket error: %d\n", getpid(), errno);

        return SOCK_SERV_ERROR;
    }

    //reuse port
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, (const void *) &reuseport, sizeof(int)) == -1) {
        printf("[%d], Failed to set reuse port : %d\n", getpid(), errno);
    }

    //set non blocking
    if (fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFD, 0) | O_NONBLOCK) == -1) {
        printf("[%d], Set non blocking error : %d\n", getpid(), errno);

        return SOCK_SERV_ERROR;
    }

    //bind
    if (bind(listen_fd, ( struct sockaddr *)server_addr, sizeof(struct sockaddr)) == -1) {
        printf("[%d], Failed to bind : %d\n", getpid(), errno);

        return SOCK_SERV_ERROR;
    }

    if (listen(listen_fd, MAX_BACK_LOG) == -1) {
        printf("[%d], Failed to listen : %d\n", getpid(), errno);

        return SOCK_SERV_ERROR;
    }

    //using epoll
    int ep_fd = epoll_create(MAX_CONN);
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;


    if (epoll_ctl(ep_fd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        printf("[%d], Failed to add epoll event: %d\n", getpid(), errno);

        return SOCK_SERV_ERROR;
    }

    *sock_fd = listen_fd;
    *epoll_fd = ep_fd;

    printf("[%d], create sock fd : %d, epoll fd : %d\n", getpid(), listen_fd, ep_fd);

    return SOCK_SERV_SUCCESS;
}

int main(int argc, char *argv[]) {
    int listen_fd;
    int epoll_fd;

    struct rlimit rlt;
    struct sockaddr_in server_addr;

    rlt.rlim_max = rlt.rlim_cur = MAX_CONN;

    if (setrlimit(RLIMIT_NOFILE, &rlt) == -1) {
        printf("Failed to set max rlimit : %d\n", errno);

        exit(EXIT_FAILURE);
    }

    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);


    for (int i = 0; i < WORKERS; i++) {
        pid_t pid = fork();

        switch(pid){
            case 0:
                sprintf(argv[0], "socket_server: child worker");

                if (listen_sock(&listen_fd, &epoll_fd, &server_addr) == SOCK_SERV_ERROR) {
                    exit(EXIT_FAILURE);
                }

                if (fork_worker(listen_fd, epoll_fd) == SOCK_SERV_ERROR) {
                    exit(EXIT_FAILURE);
                }

                close(listen_fd);

            case -1:
                printf("Failed to fork worker process");

                exit(EXIT_FAILURE);

            default:
                waitpid(pid, NULL, WNOHANG);
        }
    }

    while (1) {
        sleep(5);
    }

    return 0;
}