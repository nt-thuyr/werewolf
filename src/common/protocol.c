#include "protocol.h"

#include <arpa/inet.h>
#include <string.h>

void protocol_put_u16(uint8_t *bytes, uint16_t value)
{
    value = htons(value);
    memcpy(bytes, &value, sizeof value);
}

void protocol_put_u32(uint8_t *bytes, uint32_t value)
{
    value = htonl(value);
    memcpy(bytes, &value, sizeof value);
}

uint16_t protocol_get_u16(const uint8_t *bytes)
{
    uint16_t value;
    memcpy(&value, bytes, sizeof value);
    return ntohs(value);
}

uint32_t protocol_get_u32(const uint8_t *bytes)
{
    uint32_t value;
    memcpy(&value, bytes, sizeof value);
    return ntohl(value);
}

void protocol_encode_header(uint8_t *bytes, const struct message_header *header)
{
    bytes[0] = header->type;
    protocol_put_u16(bytes + 1, header->length);
    protocol_put_u32(bytes + 3, header->request_id);
    bytes[7] = header->status;
}

void protocol_decode_header(struct message_header *header, const uint8_t *bytes)
{
    header->type = bytes[0];
    header->length = protocol_get_u16(bytes + 1);
    header->request_id = protocol_get_u32(bytes + 3);
    header->status = bytes[7];
}

size_t protocol_put_name(uint8_t *bytes, const char *name)
{
    size_t length = strlen(name);
    bytes[0] = (uint8_t)length;
    memcpy(bytes + 1, name, length);
    return 1 + length;
}

bool protocol_get_name(const uint8_t *body, size_t length, size_t *offset,
                       char name[PROTOCOL_MAX_NAME + 1])
{
    if (*offset >= length) {
        return false;
    }
    size_t size = body[*offset];
    size_t start = *offset + 1;
    if (size == 0 || size > PROTOCOL_MAX_NAME || size > length - start ||
        memchr(body + start, '\0', size) != NULL) {
        return false;
    }

    memcpy(name, body + start, size);
    name[size] = '\0';
    *offset = start + size;
    return true;
}
