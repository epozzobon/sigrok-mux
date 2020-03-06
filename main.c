#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include "capture.h"

#define UNUSED(x) (void)(x)
#define CLIENT_BUFF_SIZE 1000

typedef struct client_sample {
    double time;
    uint64_t value;
} client_sample_t;

typedef struct client {
    int sock;
    bool closing;
    uint64_t mask;
    client_sample_t buffer[CLIENT_BUFF_SIZE];
    size_t buffer_idx;
    LIST_ENTRY(client) entries;
} client_t;

pthread_mutex_t clients_mutex;
pthread_t clients_thread;
int server_socket;
bool exit_flag = false;

LIST_HEAD(clients_list, client) clients_head;
struct clients_list *clients;


static client_t *new_client() {
    pthread_mutex_lock(&clients_mutex);
    client_t *c = (client_t*) malloc(sizeof(*c));
    if (c == NULL) {
        perror("Failed to allocate client");
        exit(1);
    }
    memset(c, 0, sizeof(*c));
    LIST_INSERT_HEAD(&clients_head, c, entries);
    pthread_mutex_unlock(&clients_mutex);
    return c;
}


static void poll_clients() {
    fd_set readfds, writefds, exceptfds;
    if (LIST_EMPTY(&clients_head)) {
        return;
    }

    pthread_mutex_lock(&clients_mutex);

    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    FD_ZERO(&exceptfds);
    client_t *c;
    int max_socket = 0;

    LIST_FOREACH(c, &clients_head, entries) {
        int sock = c->sock;
        FD_SET(sock, &readfds);
        FD_SET(sock, &writefds);
        //FD_SET(sock, &exceptfds);
        if (sock > max_socket) {
            max_socket = sock;
        }
    }

    struct timeval timeout = { 0 };
    int res = select(max_socket+1, &readfds, NULL, NULL, &timeout);
    if (res == -1) {
        perror("select clients failed");
    }

    LIST_FOREACH(c, &clients_head, entries) {
        int sock = c->sock;
        size_t buf_size = sizeof(c->buffer[0]) * c->buffer_idx;
        
        if (FD_ISSET(sock, &exceptfds)) {
            uint8_t buf[16];
            ssize_t recv_r = recv(sock, buf, 16, MSG_OOB | MSG_DONTWAIT);
            if (recv_r == -1) {
                perror("recv MSG_OOB failed");
                c->closing = true;
            } else {
                fprintf(stderr, "Client %d sent OOB data:", sock);
                for (unsigned int i = 0; i < recv_r; i++) {
                    fprintf(stderr, " %02x", buf[i]);
                }
                fprintf(stderr, "\n");
            }
        }

        if (FD_ISSET(sock, &readfds)) {
            ssize_t recv_r = recv(sock, &c->mask, sizeof(c->mask), MSG_DONTWAIT);
            if (recv_r == sizeof(c->mask)) {
                fprintf(stderr, "Client %d set mask %lx\n", sock, c->mask);
            } else {
                c->closing = true;
                if (recv_r == 0) {
                    fprintf(stderr, "Client %d disconnected\n", sock);
                } else if (recv_r == -1) {
                    perror("recv failed");
                } else {
                    fprintf(stderr, "Unexpected read from client %d\n", sock);
                }
            }
        }

        if (buf_size > 0 && FD_ISSET(sock, &writefds)) {
            ssize_t send_r = send(sock, c->buffer, buf_size, MSG_DONTWAIT);
            if ((size_t) send_r == buf_size) {
                c->buffer_idx = 0;
            } else {
                c->closing = true;
                if (send_r == -1) {
                    perror("send failed");
                } else {
                    fprintf(stderr, "Client %d couldn't keep up!\n", sock);
                }
            }
        }
    }

    LIST_FOREACH(c, &clients_head, entries) {
        if (c->closing) {
            fprintf(stderr, "Client %d closing\n", c->sock);
            if (-1 == close(c->sock)) {
                perror("close failed");
            }
            LIST_REMOVE(c, entries);
            free(c);
        }
    }

    pthread_mutex_unlock(&clients_mutex);
}


void on_capture_change(double time, uint64_t prev, uint64_t unit) {
    client_t *c;
    printf("%lf, %lx, %lx\n", time, prev, unit);
    uint64_t diff = prev ^ unit;

    pthread_mutex_lock(&clients_mutex);
    LIST_FOREACH(c, &clients_head, entries) {
        uint64_t mask = c->mask;
        if (!(mask & diff)) { continue; }
        uint64_t value = unit & mask;
        if (c->buffer_idx >= CLIENT_BUFF_SIZE) {
            c->buffer_idx = 0;
            fprintf(stderr, "\nBUFFER OVERFLOW FOR CLIENT %d\n", c->sock);
            c->closing = true;
        }
        client_sample_t sample = { .time = time, .value = value };
        c->buffer[c->buffer_idx++] = sample;
    }
    pthread_mutex_unlock(&clients_mutex);
}


static void *clients_task(void *param) {
    UNUSED(param);
    int res;
    while (!exit_flag) {
        struct timeval timeout = { .tv_sec = 0, .tv_usec = 10000 };
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_socket, &readfds);
        res = select(server_socket+1, &readfds, NULL, NULL, &timeout);
        if (res == -1) {
            perror("select failed");
        }
        if (FD_ISSET(server_socket, &readfds)) {
            struct sockaddr_un cli_addr;
            socklen_t cli_addr_len = sizeof(cli_addr);
            fprintf(stderr, "Accepting client...\n");
            int cli_sock = accept(server_socket, (struct sockaddr *) &cli_addr, &cli_addr_len);
            if (cli_sock == -1) {
                perror("accept failed");
            } else {
                fprintf(stderr, "Accepted client %d\n", cli_sock);
                fprintf(stderr, "Client %d is", cli_sock);
                for (unsigned int i = 0; i < cli_addr_len && i < sizeof(cli_addr); i++) {
                    fprintf(stderr, " %02x", ((uint8_t*) &cli_addr)[i]);
                }
                fprintf(stderr, ".\n");
                client_t *c = new_client(cli_sock);
                c->sock = cli_sock;
                c->buffer_idx = 0;
                c->mask = 0xffffffff;
            }
        }
        poll_clients();
    
    }
    return NULL;
}


static void sig_handler(int signo)
{
    fprintf(stderr, "received signal %d\n", signo);
    if (signo == SIGINT || signo == SIGTERM) {
        if (!capture_stop()) {
            fprintf(stderr, "Forcing exit.\n");
            exit(1);
        }
    }
    if (signo == SIGPIPE) {
        fprintf(stderr, "Just a SIGPIPE, moving along...\n");
    }
}


int main(int argc, char **argv) {
    UNUSED(argc);
    UNUSED(argv);

    char *socket_path = "./socket";
    struct sockaddr_un addr;

    LIST_INIT(&clients_head);
    if (0 != pthread_mutex_init(&clients_mutex, NULL)) {
        fprintf(stderr, "\ncan't make mutex\n");
        exit(1);
    }

    for (int i = 0; i < argc; i++) {
        if (i == 1) {
            socket_path = argv[i];
        }
    }

    server_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (-1 == server_socket) {
        perror("socket failed");
        exit(1);
    }


    int flags = fcntl(server_socket, F_GETFL, 0);
    if (flags == -1) {
        perror("fnctl failed");
        exit(1);
    }
    flags = flags | O_NONBLOCK;
    if (0 != fcntl(server_socket, F_SETFL, flags)) {
        perror("fnctl failed");
        exit(1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path)-1);
    if (0 != strncmp(socket_path, addr.sun_path, sizeof(addr.sun_path)-1)) {
        fprintf(stderr, "\nstrncpy failed\n");
        exit(1);
    }
    
    unlink(addr.sun_path);
    if (0 != bind(server_socket, (struct sockaddr*)&addr, sizeof(addr))) {
        perror("bind failed");
        exit(1);
    }

    if (0 != listen(server_socket, 16)) {
        perror("listen failed");
        exit(1);
    }

    if (signal(SIGINT, sig_handler) == SIG_ERR) {
        fprintf(stderr, "\ncan't catch SIGINT\n");
        exit(1);
    }
    if (signal(SIGPIPE, sig_handler) == SIG_ERR) {
        fprintf(stderr, "\ncan't catch SIGPIPE\n");
        exit(1);
    }
    if (signal(SIGTERM, sig_handler) == SIG_ERR) {
        fprintf(stderr, "\ncan't catch SIGTERM\n");
        exit(1);
    }

    if (0 != pthread_create(&clients_thread, NULL, clients_task, NULL)) {
        fprintf(stderr,  "\ncan't create thread\n");
        exit(1);
    }

    capture_init();
    capture_run();
    exit_flag = 1;
    if (0 != pthread_join(clients_thread, NULL)) {
        fprintf(stderr, "\ncan't join thread\n");
        exit(1);
    }

    if (0 != pthread_mutex_destroy(&clients_mutex)) {
        fprintf(stderr, "\ncan't destroy mutex\n");
        exit(1);
    }
    
    unlink(addr.sun_path);
    exit(0);
}


