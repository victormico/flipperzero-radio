/**
 * @file TEA5767.c
 * @author Victor Micó 
 * @brief Implementation to control the TEA5767 radio chip.
 * @version 0.1
 * @date 2023-08-24
 * 
 * @copyright GPLv3
 * 
 * Ported from:
 * https://github.com/mathertel/Radio
 */
#include <furi.h>
#include <furi_hal.h>
#include <stdint.h>
#include "TEA5767.h"

bool tea5767_is_device_ready(void) {
    bool result = false;

    furi_hal_i2c_acquire(&furi_hal_i2c_handle_external);
    if(furi_hal_i2c_is_device_ready(&furi_hal_i2c_handle_external, TEA5767_ADR, 5)) {
        result = true;
    }
    furi_hal_i2c_release(&furi_hal_i2c_handle_external);
    return result;
}

bool tea5767_read_registers(uint8_t* buffer) {
    bool result = false;

    furi_hal_i2c_acquire(&furi_hal_i2c_handle_external);
    if(furi_hal_i2c_rx(&furi_hal_i2c_handle_external, TEA5767_ADR, buffer, 5, 100)) {
        result = true;
    }
    furi_hal_i2c_release(&furi_hal_i2c_handle_external);
    return result;
}

bool tea5767_write_registers(uint8_t* buffer) {
    bool result = false;

    furi_hal_i2c_acquire(&furi_hal_i2c_handle_external);
    if(furi_hal_i2c_tx(&furi_hal_i2c_handle_external, TEA5767_ADR, buffer, 5, 100)) {
        result = true;
    }
    furi_hal_i2c_release(&furi_hal_i2c_handle_external);
    return result;
}

bool tea5767_init(uint8_t* buffer) {
    bool result = false;

    buffer[0] = 0x00;
    buffer[1] = 0x00;
    buffer[2] = 0xB0;
    buffer[3] = REG_4_XTAL | REG_4_SMUTE;
    buffer[4] = 0x00;

    result = tea5767_write_registers(buffer);
    return result;
}

bool tea5767_set_mute(uint8_t* buffer, bool mute) {
    bool result = false;

    if(mute) {
        buffer[REG_1] |= REG_1_MUTE;
    } else {
        buffer[REG_1] &= ~REG_1_MUTE;
    }

    result = tea5767_write_registers(buffer);
    return result;
}

bool tea5767_set_mono(uint8_t* buffer, bool mono) {
    bool result = false;

    if(mono) {
        buffer[REG_3] |= REG_3_MS;
    } else {
        buffer[REG_3] &= ~REG_3_MS;
    }

    result = tea5767_write_registers(buffer);
    return result;
}

bool tea5767_set_standby(uint8_t* buffer, bool standby) {
    bool result = false;

    if(standby) {
        buffer[REG_4] |= REG_4_STBY;
    } else {
        buffer[REG_4] &= ~REG_4_STBY;
    }

    result = tea5767_write_registers(buffer);
    return result;
}

// PLL word for a frequency in 10 kHz units, using high side injection
static uint16_t tea5767_frequency_to_pll(int value) {
    return 4 * (value * 10000 + FILTER) / QUARTZ;
}

// Frequency in 10 kHz units for a PLL word, rounded to the closest 10 kHz
static int tea5767_pll_to_frequency(uint16_t pll) {
    return (pll * QUARTZ / 4 - FILTER + 5000) / 10000;
}

static void tea5767_set_pll(uint8_t* buffer, uint16_t pll) {
    buffer[REG_1] = (buffer[REG_1] & ~REG_1_PLL) | ((pll >> 8) & REG_1_PLL);
    buffer[REG_2] = pll & REG_2_PLL;
}

// The status bytes are read into their own buffer, so the write registers are never overwritten
bool tea5767_get_frequency(uint8_t* buffer, int* value) {
    UNUSED(buffer);
    bool result = false;
    uint8_t status[5];

    if(tea5767_read_registers(status)) {
        *value = tea5767_pll_to_frequency(((status[REG_1] & REG_1_PLL) << 8) | status[REG_2]);
        result = true;
    }
    return result;
}

bool tea5767_set_frequency(uint8_t* buffer, int value) {
    bool result = false;

    buffer[REG_1] &= ~REG_1_SM;
    tea5767_set_pll(buffer, tea5767_frequency_to_pll(value));

    result = tea5767_write_registers(buffer);
    return result;
}

bool tea5767_seek(uint8_t* buffer, int value, bool up) {
    bool result = false;

    buffer[REG_1] |= REG_1_SM;
    tea5767_set_pll(buffer, tea5767_frequency_to_pll(value));
    buffer[REG_3] &= ~(REG_3_SUD | REG_3_SSL);
    buffer[REG_3] |= REG_3_SSL_MID;
    if(up) {
        buffer[REG_3] |= REG_3_SUD;
    }

    result = tea5767_write_registers(buffer);
    return result;
}

bool tea5767_get_seek_status(bool* done, bool* band_limit, int* value) {
    bool result = false;
    uint8_t status[5];

    if(tea5767_read_registers(status)) {
        *done = (status[REG_1] & STATUS_1_RF) != 0;
        *band_limit = (status[REG_1] & STATUS_1_BLF) != 0;
        *value = tea5767_pll_to_frequency(((status[REG_1] & REG_1_PLL) << 8) | status[REG_2]);
        result = true;
    }
    return result;
}

bool tea5767_get_radio_info(uint8_t* buffer, struct RADIO_INFO* info) {
    bool result = false;
    uint8_t status[5];

    if(tea5767_read_registers(status)) {
        info->active = true;
        info->tuned = (status[REG_1] & STATUS_1_RF) != 0;
        info->stereo = (status[REG_3] & REG_3_STEREO) != 0;
        info->mono = (buffer[REG_3] & REG_3_MS) != 0;
        info->rssi = (status[REG_4] & REG_4_ADC) >> 4;
        info->snr = 0;
        info->rds = false;
        result = true;
    }
    return result;
}
