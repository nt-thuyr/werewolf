#include "protocol.h"

#include <arpa/inet.h>
#include <string.h>

void protocol_encode_header(uint8_t *bytes, const struct message_header *header)
{
    uint16_t length = htons(header->length);
    uint32_t request_id = htonl(header->request_id);

    bytes[0] = header->type;
    memcpy(bytes + 1, &length, sizeof length);
    memcpy(bytes + 3, &request_id, sizeof request_id);
    bytes[7] = header->status;
}

void protocol_decode_header(struct message_header *header, const uint8_t *bytes)
{
    uint16_t length;
    uint32_t request_id;

    memcpy(&length, bytes + 1, sizeof length);
    memcpy(&request_id, bytes + 3, sizeof request_id);
    header->type = bytes[0];
    header->length = ntohs(length);
    header->request_id = ntohl(request_id);
    header->status = bytes[7];
}
