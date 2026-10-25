/**
 * @file RDS.c
 * @brief Decoder for the RDS (Radio Data System) station name and radio text.
 * @version 0.1
 * @date 2026-10-25
 * 
 * @copyright GPLv3
 */
#include <string.h>
#include "RDS.h"

// Blocks with more corrected errors than this are ignored.
#define RDS_MAX_ERRORS 1

#define RDS_TEXT_END 0x0D

// RDS characters 0x80-0x9B and 0xC0-0xC9 are accented letters, shown without the accent.
static const char rds_chars_80[] = "aaeeiioouuNCSs!I" "aaeeiioouunc";
static const char rds_chars_c0[] = "AAEEIIOOUU";

static char rds_char(uint8_t c) {
    if(c >= 0x20 && c < 0x7F) {
        return c;
    } else if(c >= 0x80 && c < 0x80 + sizeof(rds_chars_80) - 1) {
        return rds_chars_80[c - 0x80];
    } else if(c >= 0xC0 && c < 0xC0 + sizeof(rds_chars_c0) - 1) {
        return rds_chars_c0[c - 0xC0];
    }
    return ' ';
}

// Copies the buffer into the string without leading and trailing spaces.
static void rds_copy_trimmed(char* string, const char* buffer, uint8_t length) {
    uint8_t start = 0;
    while(start < length && buffer[start] == ' ') {
        start++;
    }
    while(length > start && buffer[length - 1] == ' ') {
        length--;
    }
    memcpy(string, buffer + start, length - start);
    string[length - start] = '\0';
}

void rds_reset(RdsData* rds) {
    memset(rds, 0, sizeof(RdsData));
    memset(rds->ps_buffer, ' ', RDS_PS_LENGTH);
    memset(rds->text_buffer, ' ', RDS_TEXT_LENGTH);
    rds->text_ab = 0xFF;
}

static void rds_process_ps(RdsData* rds, uint16_t block_b, uint16_t block_d) {
    uint8_t segment = block_b & 0x03;
    rds->ps_buffer[segment * 2] = rds_char(block_d >> 8);
    rds->ps_buffer[segment * 2 + 1] = rds_char(block_d & 0xFF);
    rds->ps_segments |= 1 << segment;

    // Only show the name once all 4 segments have been received.
    if(rds->ps_segments == 0x0F) {
        rds_copy_trimmed(rds->ps, rds->ps_buffer, RDS_PS_LENGTH);
        rds->ps_segments = 0;
    }
}

static void rds_process_text(RdsData* rds, const uint16_t* blocks, bool version_b) {
    uint8_t text_ab = (blocks[1] >> 4) & 0x01;
    if(text_ab != rds->text_ab) {
        // A new text is being sent.
        memset(rds->text_buffer, ' ', RDS_TEXT_LENGTH);
        rds->text_ab = text_ab;
    }

    uint8_t segment = blocks[1] & 0x0F;
    if(version_b) {
        rds->text_buffer[segment * 2] = blocks[3] >> 8;
        rds->text_buffer[segment * 2 + 1] = blocks[3] & 0xFF;
    } else {
        rds->text_buffer[segment * 4] = blocks[2] >> 8;
        rds->text_buffer[segment * 4 + 1] = blocks[2] & 0xFF;
        rds->text_buffer[segment * 4 + 2] = blocks[3] >> 8;
        rds->text_buffer[segment * 4 + 3] = blocks[3] & 0xFF;
    }

    char text[RDS_TEXT_LENGTH];
    uint8_t length = 0;
    while(length < RDS_TEXT_LENGTH && rds->text_buffer[length] != RDS_TEXT_END) {
        text[length] = rds_char(rds->text_buffer[length]);
        length++;
    }
    rds_copy_trimmed(rds->text, text, length);
}

void rds_process(RdsData* rds, const uint16_t* blocks, const uint8_t* errors) {
    if(errors[0] <= RDS_MAX_ERRORS && blocks[0] != rds->pi) {
        // Another station, forget what was received from the previous one.
        if(rds->pi != 0) {
            rds_reset(rds);
        }
        rds->pi = blocks[0];
    }
    if(errors[1] > RDS_MAX_ERRORS || errors[3] > RDS_MAX_ERRORS) {
        return;
    }

    uint8_t group = blocks[1] >> 12;
    bool version_b = (blocks[1] & 0x0800) != 0;
    if(group == 0) {
        rds_process_ps(rds, blocks[1], blocks[3]);
    } else if(group == 2 && (version_b || errors[2] <= RDS_MAX_ERRORS)) {
        rds_process_text(rds, blocks, version_b);
    }
}
