/**
 * @file RDS.h
 * @brief Decoder for the RDS (Radio Data System) station name and radio text.
 * @version 0.1
 * @date 2026-10-25
 * 
 * @copyright GPLv3
 * 
 * Based on the IEC 62106 standard, groups 0A/0B (programme service name)
 * and 2A/2B (radio text).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RDS_PS_LENGTH 8
#define RDS_TEXT_LENGTH 64

typedef struct {
    uint16_t pi; ///< Programme identification code, 0 when unknown.
    char ps[RDS_PS_LENGTH + 1]; ///< Station name, empty until fully received.
    char text[RDS_TEXT_LENGTH + 1]; ///< Radio text, empty until received.
    // Decoder state
    char ps_buffer[RDS_PS_LENGTH];
    uint8_t ps_segments;
    char text_buffer[RDS_TEXT_LENGTH];
    uint8_t text_ab;
} RdsData;

/**
 * @brief Forget everything received, call it when the station changes
 */
void rds_reset(RdsData* rds);

/**
 * @brief Decode an RDS group
 * @param blocks the 4 blocks of the group
 * @param errors errors of each block: 0 none, 1 = 1-2 corrected, 2 = 3-5 corrected, 3 = uncorrectable
 */
void rds_process(RdsData* rds, const uint16_t* blocks, const uint8_t* errors);
