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

// Конфигурация для максимальной боли
#define RING_ENTRIES 8192
#define CONNS_PER_THREAD 1000
#define NUM_THREADS 8 // Настрой под количество физических ядер
#define REQ_SIZE 128
#define RESP_SIZE 1024

// Жестко закодированный HTTP запрос для максимальной скорости
const char *http_req = 
    "GET / HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "Connection: keep-alive\r\n"
    "Accept: */*\r\n\r\n";

int req_len;

// Состояния нашего автомата
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

// Привязываем поток к конкретному ядру для убийства кэш-промахов
void pin_thread_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

// Настройка сокета: неблокирующий, без задержки TCP
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
    // Используем предкомпилированный запрос, 0 аллокаций
    io_uring_prep_send(sqe, conn->fd, http_req, req_len, 0);
    conn->state = STATE_SEND;
    io_uring_sqe_set_data(sqe, conn);
}

void add_recv(struct io_uring *ring, struct connection *conn) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_recv(sqe, conn->fd, conn->recv_buf, RESP_SIZE, 0);
    conn->state = STATE_RECV;
    io_uring_sqe_set_data(sqe, conn);
}

void *worker_thread(void *arg) {
    struct thread_data *td = (struct thread_data *)arg;
    pin_thread_to_core(td->thread_id);

    // Магия: IORING_SETUP_SQPOLL означает, что ядро создает отдельный поток 
    // для поллинга очереди. Нам даже не нужно вызывать системные вызовы для отправки!
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SQPOLL | IORING_SETUP_CQSIZE;
    params.cq_entries = RING_ENTRIES * 2;
    params.sq_thread_idle = 2000;

    if (io_uring_queue_init_params(RING_ENTRIES, &td->ring, &params) < 0) {
        perror("io_uring_queue_init_params");
        return NULL;
    }

    // Инициализируем соединения
    for (int i = 0; i < CONNS_PER_THREAD; i++) {
        td->conns[i].fd = setup_socket();
        add_connect(&td->ring, &td->conns[i], &td->target_addr);
    }
    
    // Пакетная отправка первой волны
    io_uring_submit(&td->ring);

    struct io_uring_cqe *cqe;
    while (1) {
        // Читаем завершенные события
        unsigned head;
        int count = 0;
        
        io_uring_for_each_cqe(&td->ring, head, cqe) {
            struct connection *conn = (struct connection *)io_uring_cqe_get_data(cqe);
            int res = cqe->res;
            
            if (res < 0 && res != -EINPROGRESS && res != -EAGAIN) {
                // Если сокет умер, пересоздаем его на лету. Никакой жалости.
                close(conn->fd);
                conn->fd = setup_socket();
                add_connect(&td->ring, conn, &td->target_addr);
            } else {
                // Конечный автомат: Подключились -> Отправили -> Получили -> Отправили снова
                if (conn->state == STATE_CONNECT) {
                    add_send(&td->ring, conn);
                } else if (conn->state == STATE_SEND) {
                    add_recv(&td->ring, conn);
                } else if (conn->state == STATE_RECV) {
                    // Игнорируем ответ, мы здесь чтобы забивать канал
                    add_send(&td->ring, conn);
                }
            }
            count++;
        }
        
        if (count > 0) {
            io_uring_cq_advance(&td->ring, count);
            // Submit только если SQPOLL спит (крайне редко при такой нагрузке)
            io_uring_submit(&td->ring);
        }
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <IP> <PORT>\n", argv[0]);
        return 1;
    }

    req_len = strlen(http_req);
    pthread_t threads[NUM_THREADS];
    struct thread_data td[NUM_THREADS];

    struct sockaddr_in target;
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons(atoi(argv[2]));
    inet_pton(AF_INET, argv[1], &target.sin_addr);

    printf("[ENI] Запускаю генератор бездны на %d потоках...\n", NUM_THREADS);

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
