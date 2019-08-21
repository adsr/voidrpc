#ifndef _VOIDRPC_H
#define _VOIDRPC_H

/* voidrpc - lightweight content-agnostic RPC library
 *
 * It is essentially a wrapper for read(2), write(2), and some dynamic buffers
 * managed by realloc(3). Feed it any kind of read/write-able file descriptors.
 * Pipes, FIFOs, sockets, blocking, non-blocking, etc.
 *
 * As the name suggests, it uses void pointers. Send messages in JSON, msgpack,
 * protobuf, raw structs, anything you want. Try not to segfault.
 *
 * The calling code tells voidrpc when to read (voidrpc_read) and write
 * (voidrpc_write). Therefore it is best if the calling code has its own event
 * loop, or otherwise can ensure it is safe to block, or otherwise checks EAGAIN
 * and sleeps or something? You probably want an event loop.
 *
 * First call voidrpc_init, then use voidrpc_send to send a remote message. Note
 * that this only queues the message in tx_buf. It is not actually sent to the
 * peer (i.e., written to wfd) until voidrpc_write is invoked. Both sides need
 * to call voidrpc_write and voidrpc_read to send and receive messages. At the
 * end, call voidrpc_free to free underlying buffers.
 * 
 * See example.c for a select(2)-based example.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

typedef struct _voidrpc_t voidrpc_t;
typedef void (*voidrpc_extract_msg_fn)(voidrpc_t *v, void *rx_buf, size_t rx_buf_len, void **msg, size_t *msg_len);
typedef void (*voidrpc_handle_msg_fn)(voidrpc_t *v, void *msg, size_t msg_len);

struct _voidrpc_t {
    int rfd;
    int wfd;
    voidrpc_extract_msg_fn fn_extract_msg;
    voidrpc_handle_msg_fn fn_handle_msg;
    void *rx_buf;
    size_t rx_buf_len;
    size_t rx_buf_size;
    void *tx_buf;
    size_t tx_buf_len;
    size_t tx_buf_size;
    int last_errno;
};


/* Initialize the struct with a read fd, a write fd, and callbacks. Note that
 * these can be the same file descriptor, e.g., when using a socket.
 *
 * fn_extract_msg: Given rx_buf, set msg and msg_len to the first complete
 * msg in rx_buf. If there is no complete msg, set msg_len to zero.
 *
 * fn_handle_msg: Process msg. Use voidrpc_send to send a response if desired.
 * This can be asynchronous.
 */
void voidrpc_init(voidrpc_t *v, int rfd, int wfd, voidrpc_extract_msg_fn fn_extract_msg, voidrpc_handle_msg_fn fn_handle_msg);

/* Add a msg to the queue. The msg is written to wfd in voidrpc_write.
 * Returns 0 on success.
 */
int voidrpc_send(voidrpc_t *v, void *tx, size_t tx_len);

/* Write to wfd. This will block unless wfd is writeable or O_NONBLOCK.
 * Returns 0 on success.
 */
int voidrpc_write(voidrpc_t *v);

/* Read from rfd. This will block unless rfd is readable or O_NONBLOCK.
 * Returns 0 on success.
 */
int voidrpc_read(voidrpc_t *v, size_t rx_len);

/* Free internal buffers if they were allocated. */
void voidrpc_free(voidrpc_t *v);


/* ======================
 * Implementation follows
 * ======================
 */

static int voidrpc_ensure_buf(voidrpc_t *v, void **buf, size_t *buf_size, size_t new_buf_size);

void voidrpc_init(voidrpc_t *v, int rfd, int wfd, voidrpc_extract_msg_fn fn_extract_msg, voidrpc_handle_msg_fn fn_handle_msg) {
    memset(v, 0, sizeof(voidrpc_t));
    v->rfd = rfd;
    v->wfd = wfd;
    v->fn_extract_msg = fn_extract_msg;
    v->fn_handle_msg = fn_handle_msg;
}

int voidrpc_send(voidrpc_t *v, void *tx, size_t tx_len) {
    if (tx_len < 1) {
        /* Nothing to send */
        return 0;
    }

    /* Ensure we have enough room in tx_buf */
    if (voidrpc_ensure_buf(v, &v->tx_buf, &v->tx_buf_size, v->tx_buf_len + tx_len) != 0) {
        /* Alloc failed */
        return -1;
    }

    /* Copy tx into tx_buf */
    memcpy(v->tx_buf + v->tx_buf_len, tx, tx_len);
    v->tx_buf_len += tx_len;

    return 0;
}

int voidrpc_write(voidrpc_t *v) {
    ssize_t rv;

    if (v->tx_buf_len < 1) {
        /* Nothing to write */
        return 0;
    }

    /* Write to wfd */
    rv = write(v->wfd, v->tx_buf, v->tx_buf_len);
    if (rv < 0) {
        /* Write failed */
        v->last_errno = errno;
        return -1;
    }

    if (rv > 0) {
        if (v->tx_buf_len - rv > 0) {
            /* There is more data left in tx_buf. Shift down */
            memmove(v->tx_buf, v->tx_buf + rv, v->tx_buf_len - rv);
            v->tx_buf_len -= rv;
        } else {
            /* There is no more data left in tx_buf */
            v->tx_buf_len = 0;
        }
    }

    return 0;
}

int voidrpc_read(voidrpc_t *v, size_t rx_len) {
    ssize_t rv;
    void *msg;
    size_t msg_len;
    size_t extract_len;
    int count;

    /* Ensure we have enough room in rx_buf */
    if (voidrpc_ensure_buf(v, &v->rx_buf, &v->rx_buf_size, v->rx_buf_len + rx_len) != 0) {
        /* Alloc failed */
        return -1;
    }

    /* Read from rfd */
    rv = read(v->rfd, v->rx_buf + v->rx_buf_len, rx_len);
    if (rv < 0) {
        /* Read failed */
        v->last_errno = errno;
        return -1;
    }
    v->rx_buf_len += rv;

    count = 0;
    while (1) {
        /* Is there a complete msg in the buffer? */
        msg = NULL;
        msg_len = 0;
        (v->fn_extract_msg)(v, v->rx_buf, v->rx_buf_len, &msg, &msg_len);
        if (msg_len < 1) {
            break;
        }

        /* Invoke procedure */
        (v->fn_handle_msg)(v, msg, msg_len);
        count += 1;

        /* Now we shift down whatever was left over in rx_buf. fn_extract_msg
         * can return a pointer that is not at the beginning of rx_buf, so we
         * need to skip over that PLUS the length of the msg.
         */
        extract_len = (msg - v->rx_buf) + msg_len;
        if (v->rx_buf_len - extract_len > 0) {
            /* There is left over data in rx_buf. Shift down */
            memmove(v->rx_buf, v->rx_buf + extract_len, v->rx_buf_len - extract_len);
            v->rx_buf_len -= extract_len;
        } else {
            /* There is no more data left in rx_buf */
            v->rx_buf_len = 0;
        }
    }

    return count;
}

void voidrpc_free(voidrpc_t *v) {
    if (v->tx_buf) free(v->tx_buf);
    if (v->rx_buf) free(v->rx_buf);
}

static int voidrpc_ensure_buf(voidrpc_t *v, void **buf, size_t *buf_size, size_t new_buf_size) {
    if (new_buf_size > *buf_size) {
        *buf = realloc(*buf, new_buf_size);
        if (!*buf) {
            v->last_errno = ENOMEM;
            return -1;
        }
        *buf_size = new_buf_size;
    }
    return 0;
}

#endif
