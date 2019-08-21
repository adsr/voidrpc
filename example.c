#include <stdio.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include "voidrpc.h"

typedef struct _msg_t msg_t;

struct _msg_t {
    voidrpc_t *rpc;
    int id;
    char note[64];
};

#define NUM_TASKS 5

static pthread_t child_thread[NUM_TASKS];
static pthread_mutex_t child_mutex = PTHREAD_MUTEX_INITIALIZER;

static void example_extract_call(voidrpc_t *rpc, void *rx_buf, size_t rx_buf_len, void **call, size_t *call_len);
static void handle_parent(voidrpc_t *rpc, void *call, size_t call_len);
static void handle_child(voidrpc_t *rpc, void *call, size_t call_len);
static void *child_run(void *arg);

static int done = 0;
static int seen = 0;

int main(int argc, char **argv) {
    voidrpc_t rpc;
    pid_t pid;
    int child_to_parent[2];
    int parent_to_child[2];
    struct timeval tv;
    int rfd, wfd, maxfd;
    fd_set rfds, wfds;
    msg_t msg;
    int rv;
    int i;

    srand(time(NULL));

    if (pipe(child_to_parent) != 0) {
        perror("pipe");
        return 1;
    }
    if (pipe(parent_to_child) != 0) {
        perror("pipe");
        return 1;
    }

    pid = fork();
    if (pid < 0) {
        /* Fork failed */
        perror("fork");
        return 1;
    } else if (pid == 0) {
        /* Child */
        rfd = parent_to_child[0];
        wfd = child_to_parent[1];
        voidrpc_init(&rpc, rfd, wfd, example_extract_call, handle_child);
    } else {
        /* Parent */
        rfd = child_to_parent[0];
        wfd = parent_to_child[1];
        voidrpc_init(&rpc, rfd, wfd, example_extract_call, handle_parent);

        /* Queue some calls */
        for (i = 0; i < NUM_TASKS; ++i) {
            msg.id = i;
            voidrpc_send(&rpc, &msg, sizeof(msg_t));
        }
    }

    while (!done || rpc.tx_buf_len > 0) {
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_SET(rfd, &rfds);
        maxfd = rfd;
        if (rpc.tx_buf_len > 0) {
            FD_SET(wfd, &wfds);
            if (wfd > maxfd) maxfd = wfd;
        }
        rv = select(maxfd + 1, &rfds, &wfds, NULL, &tv);
        if (rv == -1) {
            perror("select");
            break;
        }
        if (FD_ISSET(rfd, &rfds)) {
            voidrpc_read(&rpc, 180);
        }
        if (FD_ISSET(wfd, &wfds)) {
            voidrpc_write(&rpc);
        }
    }

    if (pid == 0) {
        for (i = 0; i < NUM_TASKS; ++i) {
            pthread_join(child_thread[i], NULL);
        }
    }

main_done:
    close(rfd);
    close(wfd);
    voidrpc_free(&rpc);
}

static void example_extract_call(voidrpc_t *rpc, void *rx_buf, size_t rx_buf_len, void **call, size_t *call_len) {
    if (rx_buf_len >= sizeof(msg_t)) {
        *call = rx_buf;
        *call_len = sizeof(msg_t);
    } else {
        *call_len = 0;
    }
}

static void handle_parent(voidrpc_t *rpc, void *call, size_t call_len) {
    msg_t *msg;
    pid_t pid;

    msg = (msg_t*)call;

    printf("parent: msg from child: %s\n", msg->note);

    if (++seen == NUM_TASKS) done = 1;
}

static void handle_child(voidrpc_t *rpc, void *call, size_t call_len) {
    msg_t *msg;

    msg = malloc(sizeof(msg_t));
    *msg = *((msg_t *)call);
    msg->rpc = rpc;

    printf("child: spawning thread for incoming msg id %d\n", msg->id);

    pthread_create(&child_thread[msg->id], NULL, child_run, msg);
}

static void *child_run(void *arg) {
    int sleep_s;
    msg_t *msg;

    msg = (msg_t*)arg;
    sleep_s = rand() % 10;

    sleep(sleep_s);
    sprintf(msg->note, "I am child %d responding to request id %d and I slept %d seconds", (int)syscall(__NR_gettid), msg->id, sleep_s);

    pthread_mutex_lock(&child_mutex);
    voidrpc_send(msg->rpc, msg, sizeof(msg_t));
    if (++seen == NUM_TASKS) done = 1;
    pthread_mutex_unlock(&child_mutex);

    free(msg);

    return NULL;
}
