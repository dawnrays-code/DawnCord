#include "protocol.h"
#include <string.h>
#include <arpa/inet.h>

int protocol_encode(dawncord_msg_type type, const char *json, size_t json_len,
                    uint8_t *out, size_t out_size)
{
    if (out_size < PROTOCOL_HEADER_SIZE + json_len)
        return -1;

    uint16_t net_type = htons((uint16_t)type);
    uint32_t net_len = htonl((uint32_t)json_len);

    memcpy(out, &net_type, 2);
    memcpy(out + 2, &net_len, 4);
    memcpy(out + PROTOCOL_HEADER_SIZE, json, json_len);

    return (int)(PROTOCOL_HEADER_SIZE + json_len);
}

int protocol_decode_header(const uint8_t *buf, dawncord_msg_type *type, uint32_t *length)
{
    uint16_t net_type;
    uint32_t net_len;

    memcpy(&net_type, buf, 2);
    memcpy(&net_len, buf + 2, 4);

    *type = (dawncord_msg_type)ntohs(net_type);
    *length = ntohl(net_len);

    return 0;
}
