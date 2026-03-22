#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/epoll.h>

#define MAX_EVENTS 4096
#define BATCH_SIZE 128
#define REQ "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"

struct thread_args {
    char *ip;
    int port;
    int num_conns;
};

void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int create_conn(char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    set_nonblock(fd);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    return fd;
}

void *worker(void *arg) {
    struct thread_args *args = (struct thread_args *)arg;
    int epfd = epoll_create1(0);
    struct epoll_event ev, events[MAX_EVENTS];

    for (int i = 0; i < args->num_conns; i++) {
        int fd = create_conn(args->ip, args->port);
        if (fd >= 0) {
            ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
            ev.data.fd = fd;
            epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
        }
    }

    struct mmsghdr msgs[BATCH_SIZE];
    struct iovec iovs[BATCH_SIZE];
    for (int i = 0; i < BATCH_SIZE; i++) {
        iovs[i].iov_base = REQ;
        iovs[i].iov_len = sizeof(REQ) - 1;
        memset(&msgs[i], 0, sizeof(msgs[i]));
        msgs[i].msg_hdr.msg_iov = &iovs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }

    char read_buf[16384];

    while (1) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (events[i].events & EPOLLIN) {
                while (recv(fd, read_buf, sizeof(read_buf), MSG_DONTWAIT) > 0);
            }

            if (events[i].events & EPOLLOUT) {
                while (sendmmsg(fd, msgs, BATCH_SIZE, MSG_DONTWAIT) > 0);
            }

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
                int new_fd = create_conn(args->ip, args->port);
                if (new_fd >= 0) {
                    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
                    ev.data.fd = new_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, new_fd, &ev);
                }
            }
        }
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        return 1;
    }

    char *ip = argv[1];
    int port = atoi(argv[2]);
    int num_threads = atoi(argv[3]);
    int conns_per_thread = atoi(argv[4]);

    pthread_t *threads = malloc(num_threads * sizeof(pthread_t));
    struct thread_args args = {ip, port, conns_per_thread};

    for (int i = 0; i < num_threads; i++) {
        pthread_create(&threads[i], NULL, worker, &args);
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    free(threads);
    return 0;
}
