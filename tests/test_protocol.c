#include "cvite/protocol.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(CONDITION)                                                        \
    do {                                                                        \
        if (!(CONDITION)) {                                                     \
            (void)fprintf(                                                      \
                stderr, "CHECK failed at %s:%d: %s\n",                        \
                __FILE__, __LINE__, #CONDITION);                                \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static void write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value & 0xffU);
    bytes[1] = (uint8_t)((value >> 8U) & 0xffU);
}

static void write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xffU);
    bytes[1] = (uint8_t)((value >> 8U) & 0xffU);
    bytes[2] = (uint8_t)((value >> 16U) & 0xffU);
    bytes[3] = (uint8_t)((value >> 24U) & 0xffU);
}

int main(void)
{
    enum { packet_size = CVITE_PROTOCOL_HEADER_SIZE + 24 };
    uint8_t packet[packet_size];
    cvite_protocol_header header;
    cvite_protocol_header decoded;
    cvite_protocol_cursor cursor;
    cvite_protocol_record_view record;
    cvite_status status = CVITE_STATUS_OK;

    memset(packet, 0, sizeof(packet));

    write_u16(&packet[64], CVITE_PROTOCOL_RECORD_MANIFEST);
    write_u16(&packet[66], 0U);
    write_u32(&packet[68], 8U);

    write_u16(&packet[72], CVITE_PROTOCOL_RECORD_OBJECT);
    write_u16(&packet[74], CVITE_PROTOCOL_RECORD_FLAG_CRITICAL);
    write_u32(&packet[76], 16U);
    packet[80] = 0xaaU;
    packet[81] = 0xbbU;

    memset(&header, 0, sizeof(header));
    header.version_major = CVITE_PROTOCOL_VERSION_MAJOR;
    header.version_minor = CVITE_PROTOCOL_VERSION_MINOR;
    header.flags = CVITE_PROTOCOL_FLAG_CHECKSUM;
    header.base_generation = 4U;
    header.candidate_generation = 5U;
    header.record_count = 2U;
    header.records_size = 24U;
    header.payload_size = 0U;
    header.checksum = cvite_protocol_checksum(
        packet + CVITE_PROTOCOL_HEADER_SIZE,
        sizeof(packet) - CVITE_PROTOCOL_HEADER_SIZE);

    CHECK(cvite_protocol_encode_header(packet, &header) == CVITE_STATUS_OK);
    CHECK(cvite_protocol_validate_packet(packet, sizeof(packet), &decoded) ==
        CVITE_STATUS_OK);
    CHECK(decoded.base_generation == 4U);
    CHECK(decoded.candidate_generation == 5U);
    CHECK(decoded.record_count == 2U);

    CHECK(cvite_protocol_cursor_begin(packet, sizeof(packet), &cursor) ==
        CVITE_STATUS_OK);
    CHECK(cvite_protocol_cursor_next(&cursor, &record) == CVITE_STATUS_OK);
    CHECK(record.type == CVITE_PROTOCOL_RECORD_MANIFEST);
    CHECK(record.data_size == 0U);
    CHECK(cvite_protocol_cursor_next(&cursor, &record) == CVITE_STATUS_OK);
    CHECK(record.type == CVITE_PROTOCOL_RECORD_OBJECT);
    CHECK(record.data_size == 8U);
    CHECK(record.data[0] == 0xaaU);
    CHECK(cursor.records_remaining == 0U);
    CHECK(cursor.remaining == 0U);

    packet[80] ^= 1U;
    CHECK(cvite_protocol_validate_packet(packet, sizeof(packet), NULL) ==
        CVITE_STATUS_CHECKSUM_MISMATCH);
    packet[80] ^= 1U;

    CHECK(cvite_protocol_validate_packet(packet, sizeof(packet) - 1U, NULL) ==
        CVITE_STATUS_INVALID_PACKET);

    packet[0] = (uint8_t)'X';
    CHECK(cvite_protocol_validate_packet(packet, sizeof(packet), NULL) ==
        CVITE_STATUS_INVALID_PACKET);
    packet[0] = (uint8_t)'C';

    header.record_count = 2U;
    write_u16(&packet[72], 99U);
    header.checksum = cvite_protocol_checksum(
        packet + CVITE_PROTOCOL_HEADER_SIZE,
        sizeof(packet) - CVITE_PROTOCOL_HEADER_SIZE);
    CHECK(cvite_protocol_encode_header(packet, &header) == CVITE_STATUS_OK);
    CHECK(cvite_protocol_validate_packet(packet, sizeof(packet), NULL) ==
        CVITE_STATUS_UNSUPPORTED_PROTOCOL);
    write_u16(&packet[72], CVITE_PROTOCOL_RECORD_OBJECT);

    packet[32] = 3U;
    header.record_count = 3U;
    header.checksum = cvite_protocol_checksum(
        packet + CVITE_PROTOCOL_HEADER_SIZE,
        sizeof(packet) - CVITE_PROTOCOL_HEADER_SIZE);
    status = cvite_protocol_encode_header(packet, &header);
    CHECK(status == CVITE_STATUS_OK);
    CHECK(cvite_protocol_validate_packet(packet, sizeof(packet), NULL) ==
        CVITE_STATUS_INVALID_PACKET);

    return 0;
}
