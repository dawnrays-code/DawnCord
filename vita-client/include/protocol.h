#ifndef DAWNCORD_PROTOCOL_H
#define DAWNCORD_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define PROTOCOL_HEADER_SIZE 6
#define PROTOCOL_MAX_PAYLOAD (64 * 1024)

typedef enum {
    MSG_HANDSHAKE        = 0x0001,
    MSG_HANDSHAKE_ACK    = 0x0002,
    MSG_GUILD_LIST       = 0x0010,
    MSG_CHANNEL_LIST     = 0x0011,
    MSG_MESSAGE_LIST     = 0x0012,
    MSG_MESSAGE_NEW      = 0x0013,
    MSG_MESSAGE_SENT_ACK = 0x0014,
    MSG_IMAGE_DATA       = 0x0015,  /* {"key":..., "data": base64 jpeg|null} */
    MSG_MEMBER_LIST      = 0x0016,  /* {"channel_id":..., "members":[...]} */
    MSG_TYPING           = 0x0017,  /* {"channel_id":..., "name":...} push */
    MSG_SEND_MESSAGE     = 0x0020,
    MSG_REQUEST_GUILDS   = 0x0021,
    MSG_REQUEST_CHANNELS = 0x0022,
    MSG_REQUEST_MESSAGES = 0x0023,
    MSG_SET_CHANNEL      = 0x0024,
    MSG_REQUEST_IMAGE    = 0x0025,  /* {"url":..., "key":..., "size":...} */
    MSG_REQUEST_MEMBERS  = 0x0026,  /* {"channel_id":...} */
    MSG_ERROR            = 0x00FF,
} dawncord_msg_type;

typedef struct {
    dawncord_msg_type type;
    uint32_t length;
    char *payload;
} dawncord_message;

int protocol_encode(dawncord_msg_type type, const char *json, size_t json_len,
                    uint8_t *out, size_t out_size);

int protocol_decode_header(const uint8_t *buf, dawncord_msg_type *type, uint32_t *length);

#endif
