#include "cvite/protocol.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>

static const uint8_t cvite_magic[8] = {
    (uint8_t)'C', (uint8_t)'V', (uint8_t)'I', (uint8_t)'T',
    (uint8_t)'E', (uint8_t)'P', (uint8_t)'K', (uint8_t)'G'
};

static uint16_t cvite_read_u16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U));
}

static uint32_t cvite_read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8U) |
        ((uint32_t)bytes[2] << 16U) |
        ((uint32_t)bytes[3] << 24U);
}

static uint64_t cvite_read_u64(const uint8_t *bytes)
{
    uint64_t value = 0U;
    unsigned shift = 0U;
    size_t index = 0U;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << shift;
        shift += 8U;
    }

    return value;
}

static void cvite_write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value & 0xffU);
    bytes[1] = (uint8_t)((value >> 8U) & 0xffU);
}

static void cvite_write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xffU);
    bytes[1] = (uint8_t)((value >> 8U) & 0xffU);
    bytes[2] = (uint8_t)((value >> 16U) & 0xffU);
    bytes[3] = (uint8_t)((value >> 24U) & 0xffU);
}

static void cvite_write_u64(uint8_t *bytes, uint64_t value)
{
    size_t index = 0U;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (uint8_t)(value & 0xffU);
        value >>= 8U;
    }
}

static bool cvite_record_type_is_known(uint16_t type)
{
    return type == CVITE_PROTOCOL_RECORD_MANIFEST ||
        type == CVITE_PROTOCOL_RECORD_OBJECT;
}

static bool cvite_add_overflows_size(size_t left, uint64_t right, size_t *sum)
{
    if (right > (uint64_t)SIZE_MAX || left > SIZE_MAX - (size_t)right) {
        return true;
    }

    *sum = left + (size_t)right;
    return false;
}

uint64_t cvite_protocol_checksum(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t index = 0U;

    for (index = 0U; index < size; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }

    return hash;
}

cvite_status cvite_protocol_encode_header(
    uint8_t output[CVITE_PROTOCOL_HEADER_SIZE],
    const cvite_protocol_header *header)
{
    if (output == NULL || header == NULL ||
        header->candidate_generation <= header->base_generation ||
        (header->flags & (uint16_t)~CVITE_PROTOCOL_KNOWN_FLAGS) != 0U) {
        return CVITE_STATUS_INVALID_ARGUMENT;
    }

    memset(output, 0, CVITE_PROTOCOL_HEADER_SIZE);
    memcpy(output, cvite_magic, sizeof(cvite_magic));
    cvite_write_u16(&output[8], header->version_major);
    cvite_write_u16(&output[10], header->version_minor);
    cvite_write_u16(&output[12], CVITE_PROTOCOL_HEADER_SIZE);
    cvite_write_u16(&output[14], header->flags);
    cvite_write_u64(&output[16], header->base_generation);
    cvite_write_u64(&output[24], header->candidate_generation);
    cvite_write_u32(&output[32], header->record_count);
    cvite_write_u64(&output[40], header->records_size);
    cvite_write_u64(&output[48], header->payload_size);
    cvite_write_u64(&output[56], header->checksum);
    return CVITE_STATUS_OK;
}

cvite_status cvite_protocol_decode_header(
    const uint8_t *packet,
    size_t packet_size,
    cvite_protocol_header *header)
{
    uint16_t encoded_header_size = 0U;

    if (packet == NULL || header == NULL || packet_size < CVITE_PROTOCOL_HEADER_SIZE) {
        return CVITE_STATUS_INVALID_ARGUMENT;
    }

    if (memcmp(packet, cvite_magic, sizeof(cvite_magic)) != 0) {
        return CVITE_STATUS_INVALID_PACKET;
    }

    memset(header, 0, sizeof(*header));
    header->version_major = cvite_read_u16(&packet[8]);
    header->version_minor = cvite_read_u16(&packet[10]);
    encoded_header_size = cvite_read_u16(&packet[12]);
    header->flags = cvite_read_u16(&packet[14]);
    header->base_generation = cvite_read_u64(&packet[16]);
    header->candidate_generation = cvite_read_u64(&packet[24]);
    header->record_count = cvite_read_u32(&packet[32]);
    header->records_size = cvite_read_u64(&packet[40]);
    header->payload_size = cvite_read_u64(&packet[48]);
    header->checksum = cvite_read_u64(&packet[56]);

    if (encoded_header_size != CVITE_PROTOCOL_HEADER_SIZE) {
        return CVITE_STATUS_INVALID_PACKET;
    }

    if (header->version_major != CVITE_PROTOCOL_VERSION_MAJOR) {
        return CVITE_STATUS_UNSUPPORTED_PROTOCOL;
    }

    if ((header->flags & (uint16_t)~CVITE_PROTOCOL_KNOWN_FLAGS) != 0U) {
        return CVITE_STATUS_UNSUPPORTED_PROTOCOL;
    }

    return CVITE_STATUS_OK;
}

cvite_status cvite_protocol_validate_packet(
    const uint8_t *packet,
    size_t packet_size,
    cvite_protocol_header *header)
{
    cvite_protocol_header decoded;
    cvite_status status = CVITE_STATUS_OK;
    size_t expected_size = CVITE_PROTOCOL_HEADER_SIZE;
    cvite_protocol_cursor cursor;
    cvite_protocol_record_view record;
    uint32_t seen_records = 0U;

    status = cvite_protocol_decode_header(packet, packet_size, &decoded);
    if (status != CVITE_STATUS_OK) {
        return status;
    }

    if (decoded.candidate_generation <= decoded.base_generation) {
        return CVITE_STATUS_INVALID_PACKET;
    }

    if (cvite_add_overflows_size(expected_size, decoded.records_size, &expected_size) ||
        cvite_add_overflows_size(expected_size, decoded.payload_size, &expected_size) ||
        expected_size != packet_size) {
        return CVITE_STATUS_INVALID_PACKET;
    }

    if ((decoded.flags & CVITE_PROTOCOL_FLAG_CHECKSUM) != 0U) {
        uint64_t actual_checksum = cvite_protocol_checksum(
            packet + CVITE_PROTOCOL_HEADER_SIZE,
            packet_size - CVITE_PROTOCOL_HEADER_SIZE);
        if (actual_checksum != decoded.checksum) {
            return CVITE_STATUS_CHECKSUM_MISMATCH;
        }
    }

    cursor.next = packet + CVITE_PROTOCOL_HEADER_SIZE;
    cursor.remaining = (size_t)decoded.records_size;
    cursor.records_remaining = decoded.record_count;

    while (cursor.records_remaining > 0U) {
        status = cvite_protocol_cursor_next(&cursor, &record);
        if (status != CVITE_STATUS_OK) {
            return status;
        }
        if (!cvite_record_type_is_known(record.type) &&
            (record.flags & CVITE_PROTOCOL_RECORD_FLAG_CRITICAL) != 0U) {
            return CVITE_STATUS_UNSUPPORTED_PROTOCOL;
        }
        ++seen_records;
    }

    if (seen_records != decoded.record_count || cursor.remaining != 0U) {
        return CVITE_STATUS_INVALID_PACKET;
    }

    if (header != NULL) {
        *header = decoded;
    }
    return CVITE_STATUS_OK;
}

cvite_status cvite_protocol_cursor_begin(
    const uint8_t *packet,
    size_t packet_size,
    cvite_protocol_cursor *cursor)
{
    cvite_protocol_header header;
    cvite_status status = CVITE_STATUS_OK;

    if (cursor == NULL) {
        return CVITE_STATUS_INVALID_ARGUMENT;
    }

    status = cvite_protocol_validate_packet(packet, packet_size, &header);
    if (status != CVITE_STATUS_OK) {
        return status;
    }

    cursor->next = packet + CVITE_PROTOCOL_HEADER_SIZE;
    cursor->remaining = (size_t)header.records_size;
    cursor->records_remaining = header.record_count;
    return CVITE_STATUS_OK;
}

cvite_status cvite_protocol_cursor_next(
    cvite_protocol_cursor *cursor,
    cvite_protocol_record_view *record)
{
    uint32_t encoded_size = 0U;

    if (cursor == NULL || record == NULL || cursor->records_remaining == 0U) {
        return CVITE_STATUS_INVALID_ARGUMENT;
    }

    if (cursor->remaining < CVITE_PROTOCOL_RECORD_HEADER_SIZE) {
        return CVITE_STATUS_INVALID_PACKET;
    }

    encoded_size = cvite_read_u32(&cursor->next[4]);
    if (encoded_size < CVITE_PROTOCOL_RECORD_HEADER_SIZE ||
        (encoded_size % 8U) != 0U ||
        (size_t)encoded_size > cursor->remaining) {
        return CVITE_STATUS_INVALID_PACKET;
    }

    record->type = cvite_read_u16(&cursor->next[0]);
    record->flags = cvite_read_u16(&cursor->next[2]);
    record->data = cursor->next + CVITE_PROTOCOL_RECORD_HEADER_SIZE;
    record->data_size = encoded_size - CVITE_PROTOCOL_RECORD_HEADER_SIZE;
    record->encoded_size = encoded_size;

    cursor->next += encoded_size;
    cursor->remaining -= encoded_size;
    cursor->records_remaining -= 1U;
    return CVITE_STATUS_OK;
}
