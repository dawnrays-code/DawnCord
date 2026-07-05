#include "network.h"

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <string.h>
#include <stdlib.h>

#include "cJSON.h"

#define NET_INIT_SIZE (1 * 1024 * 1024)

static int net_initialized = 0;

static int ensure_net_init(void)
{
    if (net_initialized)
        return 0;

    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);

    SceNetInitParam param;
    static char net_memory[NET_INIT_SIZE];
    param.memory = net_memory;
    param.size = NET_INIT_SIZE;
    param.flags = 0;
    sceNetInit(&param);

    sceNetCtlInit();
    net_initialized = 1;
    return 0;
}

int net_connect(dawncord_connection *conn, const char *host, int port)
{
    ensure_net_init();

    memset(conn, 0, sizeof(*conn));
    strncpy(conn->host, host, sizeof(conn->host) - 1);
    conn->port = port;
    conn->rx_thread = -1;
    conn->queue_mutex = -1;

    conn->sock_fd = sceNetSocket("dawncord", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (conn->sock_fd < 0)
        return -1;

    SceNetSockaddrIn addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_len = sizeof(addr);
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port = sceNetHtons(port);
    sceNetInetPton(SCE_NET_AF_INET, host, &addr.sin_addr);

    int ret = sceNetConnect(conn->sock_fd, (SceNetSockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        sceNetSocketClose(conn->sock_fd);
        return -1;
    }

    conn->connected = 1;
    return 0;
}

void net_disconnect(dawncord_connection *conn)
{
    net_stop_receiver(conn);
    if (conn->connected) {
        sceNetSocketClose(conn->sock_fd);
        conn->connected = 0;
    }
}

int net_discover(char *host, unsigned int host_size, int *port, int *needs_code)
{
    ensure_net_init();

    int s = sceNetSocket("dawncord_disc", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
    if (s < 0)
        return -1;

    int on = 1;
    sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_BROADCAST,
                     &on, sizeof(on));
    int timeout_us = 1000 * 1000;   /* one second per attempt */
    sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO,
                     &timeout_us, sizeof(timeout_us));

    SceNetSockaddrIn dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_len = sizeof(dst);
    dst.sin_family = SCE_NET_AF_INET;
    dst.sin_port = sceNetHtons(NET_DISCOVERY_PORT);
    dst.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_BROADCAST);

    int found = -1;
    for (int attempt = 0; attempt < 3 && found < 0; attempt++) {
        sceNetSendto(s, NET_DISCOVERY_MAGIC, sizeof(NET_DISCOVERY_MAGIC) - 1,
                     0, (SceNetSockaddr *)&dst, sizeof(dst));

        char buf[256];
        SceNetSockaddrIn from;
        unsigned int fromlen = sizeof(from);
        int n = sceNetRecvfrom(s, buf, sizeof(buf) - 1, 0,
                               (SceNetSockaddr *)&from, &fromlen);
        if (n <= 0)
            continue;               /* timed out: broadcast again */
        buf[n] = '\0';

        cJSON *root = cJSON_Parse(buf);
        if (!root)
            continue;
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "dawncord"))) {
            const cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "port");
            if (cJSON_IsNumber(p) && p->valuedouble > 0 &&
                p->valuedouble <= 65535)
                *port = (int)p->valuedouble;
            *needs_code = cJSON_IsTrue(
                cJSON_GetObjectItemCaseSensitive(root, "needs_code"));
            sceNetInetNtop(SCE_NET_AF_INET, &from.sin_addr, host, host_size);
            found = 0;
        }
        cJSON_Delete(root);
    }

    sceNetSocketClose(s);
    return found;
}

/* ---- framed send (loops on partial writes) ---- */

static int send_all(int fd, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int ret = sceNetSend(fd, buf + sent, len - sent, 0);
        if (ret <= 0)
            return -1;
        sent += ret;
    }
    return 0;
}

int net_send_message(dawncord_connection *conn, dawncord_msg_type type, const char *json)
{
    size_t json_len = strlen(json);
    size_t total = PROTOCOL_HEADER_SIZE + json_len;
    uint8_t *buf = malloc(total);
    if (!buf)
        return -1;

    int encoded = protocol_encode(type, json, json_len, buf, total);
    if (encoded < 0) {
        free(buf);
        return -1;
    }

    int rc = send_all(conn->sock_fd, buf, (size_t)encoded);
    free(buf);
    return rc;
}

/* ---- framed receive ---- */

static int recv_exact(int fd, void *buf, size_t len)
{
    size_t received = 0;
    while (received < len) {
        int ret = sceNetRecv(fd, (char *)buf + received, len - received, 0);
        if (ret <= 0)
            return -1;
        received += ret;
    }
    return 0;
}

int net_recv_blocking(dawncord_connection *conn, dawncord_message *msg)
{
    uint8_t header[PROTOCOL_HEADER_SIZE];
    if (recv_exact(conn->sock_fd, header, PROTOCOL_HEADER_SIZE) < 0)
        return -1;

    protocol_decode_header(header, &msg->type, &msg->length);

    if (msg->length > PROTOCOL_MAX_PAYLOAD)
        return -1;

    msg->payload = malloc(msg->length + 1);
    if (!msg->payload)
        return -1;

    if (recv_exact(conn->sock_fd, msg->payload, msg->length) < 0) {
        free(msg->payload);
        msg->payload = NULL;
        return -1;
    }

    msg->payload[msg->length] = '\0';
    return 0;
}

/* ---- background receiver thread + ring buffer ---- */

static int queue_next(int i) { return (i + 1) % NET_QUEUE_SIZE; }

static int enqueue(dawncord_connection *conn, const dawncord_message *msg)
{
    sceKernelLockMutex(conn->queue_mutex, 1, NULL);
    int next = queue_next(conn->q_tail);
    if (next == conn->q_head) {
        /* Queue full: drop the message rather than block the receiver.
           Rare in practice with NET_QUEUE_SIZE slots. */
        sceKernelUnlockMutex(conn->queue_mutex, 1);
        return -1;
    }
    conn->queue[conn->q_tail] = *msg;
    conn->q_tail = next;
    sceKernelUnlockMutex(conn->queue_mutex, 1);
    return 0;
}

int net_poll_message(dawncord_connection *conn, dawncord_message *msg)
{
    int got = 0;
    sceKernelLockMutex(conn->queue_mutex, 1, NULL);
    if (conn->q_head != conn->q_tail) {
        *msg = conn->queue[conn->q_head];
        conn->q_head = queue_next(conn->q_head);
        got = 1;
    }
    sceKernelUnlockMutex(conn->queue_mutex, 1);
    return got;
}

static int rx_thread_entry(SceSize args, void *argp)
{
    (void)args;
    dawncord_connection *conn = *(dawncord_connection **)argp;

    while (conn->rx_running) {
        dawncord_message msg;
        msg.payload = NULL;
        if (net_recv_blocking(conn, &msg) < 0) {
            /* Socket closed or error: stop the loop. */
            conn->rx_running = 0;
            break;
        }
        if (enqueue(conn, &msg) < 0)
            net_free_message(&msg); /* dropped: don't leak the payload */
    }
    return sceKernelExitDeleteThread(0);
}

int net_start_receiver(dawncord_connection *conn)
{
    conn->q_head = conn->q_tail = 0;
    conn->queue_mutex = sceKernelCreateMutex("dawncord_q", 0, 0, NULL);
    if (conn->queue_mutex < 0)
        return -1;

    conn->rx_running = 1;
    conn->rx_thread = sceKernelCreateThread("dawncord_rx", rx_thread_entry,
                                            0x10000100, 0x10000, 0, 0, NULL);
    if (conn->rx_thread < 0) {
        conn->rx_running = 0;
        sceKernelDeleteMutex(conn->queue_mutex);
        conn->queue_mutex = -1;
        return -1;
    }

    dawncord_connection *self = conn;
    sceKernelStartThread(conn->rx_thread, sizeof(self), &self);
    return 0;
}

void net_stop_receiver(dawncord_connection *conn)
{
    if (conn->rx_running) {
        conn->rx_running = 0;
        /* Closing the socket unblocks the receiver's sceNetRecv. */
        if (conn->connected)
            sceNetShutdown(conn->sock_fd, SCE_NET_SHUT_RDWR);
        sceKernelWaitThreadEnd(conn->rx_thread, NULL, NULL);
        conn->rx_thread = -1;
    }
    /* Drain any leftover queued messages so their payloads aren't leaked. */
    if (conn->queue_mutex >= 0) {
        dawncord_message leftover;
        while (net_poll_message(conn, &leftover))
            net_free_message(&leftover);
        sceKernelDeleteMutex(conn->queue_mutex);
        conn->queue_mutex = -1;
    }
}

void net_free_message(dawncord_message *msg)
{
    if (msg->payload) {
        free(msg->payload);
        msg->payload = NULL;
    }
}
