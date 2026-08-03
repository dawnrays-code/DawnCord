#ifndef DAWNCORD_NETWORK_H
#define DAWNCORD_NETWORK_H

#include "protocol.h"
#include <psp2/kernel/threadmgr.h>

#define NET_QUEUE_SIZE 32

typedef struct {
    int sock_fd;
    char host[256];
    int port;
    int connected;

    /* Background receiver: a dedicated thread does blocking reads so the
       main render/input loop never stalls waiting on the socket. Parsed
       messages land in a small ring buffer the main loop drains each frame. */
    SceUID rx_thread;
    SceUID queue_mutex;
    volatile int rx_running;
    dawncord_message queue[NET_QUEUE_SIZE];
    volatile int q_head;
    volatile int q_tail;
} dawncord_connection;

int net_connect(dawncord_connection *conn, const char *host, int port);
void net_disconnect(dawncord_connection *conn);

/* LAN auto-discovery: broadcast a probe and wait for the companion's UDP
   reply. On success writes the companion's IP into host, its TCP port into
   *port and whether it requires a pairing code into *needs_code; returns 0.
   Returns -1 if nothing answered (about 3 seconds). */
#define NET_DISCOVERY_PORT  9101
#define NET_DISCOVERY_MAGIC "DAWNCORD_DISCOVER"
int net_discover(char *host, unsigned int host_size, int *port, int *needs_code);

/* Blocking send. Loops until the whole frame is written (handles partial
   sends for larger payloads like message history). Returns 0 on success. */
int net_send_message(dawncord_connection *conn, dawncord_msg_type type, const char *json);

/* Start/stop the background receiver thread. */
int net_start_receiver(dawncord_connection *conn);
void net_stop_receiver(dawncord_connection *conn);

/* Non-blocking: pops one queued message into *msg. Returns 1 if a message
   was dequeued (caller must net_free_message it), 0 if the queue is empty. */
int net_poll_message(dawncord_connection *conn, dawncord_message *msg);

/* Blocking single-message receive (used only for the initial handshake,
   before the receiver thread starts). Returns 0 on success. */
int net_recv_blocking(dawncord_connection *conn, dawncord_message *msg);

void net_free_message(dawncord_message *msg);

#endif
