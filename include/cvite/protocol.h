#ifndef CVITE_PROTOCOL_H
#define CVITE_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#include "cvite/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CVITE_PROTOCOL_VERSION_MAJOR 1U
#define CVITE_PROTOCOL_VERSION_MINOR 0U
#define CVITE_PROTOCOL_HEADER_SIZE 64U
#define CVITE_PROTOCOL_RECORD_HEADER_SIZE 8U
#define CVITE_PROTOCOL_FLAG_CHECKSUM 0x0001U
#define CVITE_PROTOCOL_KNOWN_FLAGS CVITE_PROTOCOL_FLAG_CHECKSUM
#define CVITE_PROTOCOL_RECORD_FLAG_CRITICAL 0x0001U
#define CVITE_PROTOCOL_RECORD_MANIFEST 1U
#define CVITE_PROTOCOL_RECORD_OBJECT 2U

typedef struct cvite_protocol_header {
    uint16_t version_major;
    uint16_t version_minor;
    uint16_t flags;
    uint64_t base_generation;
    uint64_t candidate_generation;
    uint32_t record_count;
    uint64_t records_size;
    uint64_t payload_size;
    uint64_t checksum;
} cvite_protocol_header;

typedef struct cvite_protocol_record_view {
    uint16_t type;
    uint16_t flags;
    const uint8_t *data;
    uint32_t data_size;
    size_t encoded_size;
} cvite_protocol_record_view;

typedef struct cvite_protocol_cursor {
    const uint8_t *next;
    size_t remaining;
    uint32_t records_remaining;
} cvite_protocol_cursor;

cvite_status cvite_protocol_encode_header(
    uint8_t output[CVITE_PROTOCOL_HEADER_SIZE],
    const cvite_protocol_header *header);

cvite_status cvite_protocol_decode_header(
    const uint8_t *packet,
    size_t packet_size,
    cvite_protocol_header *header);

cvite_status cvite_protocol_validate_packet(
    const uint8_t *packet,
    size_t packet_size,
    cvite_protocol_header *header);

cvite_status cvite_protocol_cursor_begin(
    const uint8_t *packet,
    size_t packet_size,
    cvite_protocol_cursor *cursor);

cvite_status cvite_protocol_cursor_next(
    cvite_protocol_cursor *cursor,
    cvite_protocol_record_view *record);

uint64_t cvite_protocol_checksum(const void *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif
