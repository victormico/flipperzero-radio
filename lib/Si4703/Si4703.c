/**
 * @file Si4703.c
 * @brief Implementation to control the Si4703 radio chip.
 * @version 0.1
 * @date 2026-10-25
 *
 * @copyright GPLv3
 *
 * Based on the Si4702/03 datasheet and the AN230 programming guide.
 */
#include <furi.h>
#include <furi_hal.h>
#include <stdint.h>
#include "Si4703.h"

// The Flipper doesn't enable pull-ups on the external I2C pins and most Si4703 boards have none,
// so use the internal ones. They are weak, but enough at 100 kHz with short wires.
static void si4703_i2c_acquire(void) {
    furi_hal_i2c_acquire(&furi_hal_i2c_handle_external);
    furi_hal_gpio_init_ex(
        SI4703_SCLK_PIN, GpioModeAltFunctionOpenDrain, GpioPullUp, GpioSpeedLow, GpioAltFn4I2C3);
    furi_hal_gpio_init_ex(
        SI4703_SDIO_PIN, GpioModeAltFunctionOpenDrain, GpioPullUp, GpioSpeedLow, GpioAltFn4I2C3);
}

bool si4703_reset(void) {
    // SEN is tied high, so SDIO low while RST rises selects the 2-wire (I2C) mode. SCLK is kept high.
    furi_hal_gpio_write(SI4703_SCLK_PIN, true);
    furi_hal_gpio_write(SI4703_SDIO_PIN, false);
    furi_hal_gpio_write(SI4703_RST_PIN, false);
    furi_hal_gpio_init(SI4703_SCLK_PIN, GpioModeOutputPushPull, GpioPullNo, GpioSpeedLow);
    furi_hal_gpio_init(SI4703_SDIO_PIN, GpioModeOutputPushPull, GpioPullNo, GpioSpeedLow);
    furi_hal_gpio_init(SI4703_RST_PIN, GpioModeOutputPushPull, GpioPullNo, GpioSpeedLow);
    furi_delay_ms(1);
    furi_hal_gpio_write(SI4703_RST_PIN, true);
    furi_delay_ms(1);
    // Give SCLK and SDIO back to the I2C bus, they are configured when the bus is acquired.
    furi_hal_gpio_init_simple(SI4703_SCLK_PIN, GpioModeAnalog);
    furi_hal_gpio_init_simple(SI4703_SDIO_PIN, GpioModeAnalog);

    if(!si4703_is_device_ready()) {
        furi_hal_gpio_init_simple(SI4703_RST_PIN, GpioModeAnalog);
        return false;
    }
    return true;
}

void si4703_deinit(uint16_t* registers) {
    registers[SI4703_POWERCFG] = SI4703_POWERCFG_DISABLE | SI4703_POWERCFG_ENABLE;
    registers[SI4703_SYSCONFIG1] &= ~SI4703_SYSCONFIG1_RDS;
    si4703_write_registers(registers);
    furi_hal_gpio_init_simple(SI4703_RST_PIN, GpioModeAnalog);
}

bool si4703_is_device_ready(void) {
    bool result = false;

    si4703_i2c_acquire();
    if(furi_hal_i2c_is_device_ready(&furi_hal_i2c_handle_external, SI4703_ADR, 5)) {
        result = true;
    }
    furi_hal_i2c_release(&furi_hal_i2c_handle_external);
    return result;
}

bool si4703_read_registers(uint16_t* registers) {
    bool result = false;
    uint8_t buffer[SI4703_REGISTERS * 2];

    si4703_i2c_acquire();
    if(furi_hal_i2c_rx(&furi_hal_i2c_handle_external, SI4703_ADR, buffer, sizeof(buffer), 100)) {
        // Reading always starts at register 0x0A and wraps around after 0x0F.
        for(uint8_t i = 0; i < SI4703_REGISTERS; i++) {
            registers[(SI4703_STATUSRSSI + i) % SI4703_REGISTERS] = (buffer[i * 2] << 8) |
                                                                     buffer[i * 2 + 1];
        }
        result = true;
    }
    furi_hal_i2c_release(&furi_hal_i2c_handle_external);
    return result;
}

bool si4703_write_registers(uint16_t* registers) {
    bool result = false;
    uint8_t buffer[(SI4703_TEST1 - SI4703_POWERCFG + 1) * 2];

    // Writing always starts at register 0x02.
    for(uint8_t i = 0; i < sizeof(buffer) / 2; i++) {
        buffer[i * 2] = registers[SI4703_POWERCFG + i] >> 8;
        buffer[i * 2 + 1] = registers[SI4703_POWERCFG + i] & 0xFF;
    }

    si4703_i2c_acquire();
    if(furi_hal_i2c_tx(&furi_hal_i2c_handle_external, SI4703_ADR, buffer, sizeof(buffer), 100)) {
        result = true;
    }
    furi_hal_i2c_release(&furi_hal_i2c_handle_external);
    return result;
}

bool si4703_init(uint16_t* registers) {
    if(!si4703_read_registers(registers)) {
        return false;
    }

    // Start the crystal oscillator and wait for it to settle.
    registers[SI4703_TEST1] = 0x8100;
    if(!si4703_write_registers(registers)) {
        return false;
    }
    furi_delay_ms(500);

    if(!si4703_read_registers(registers)) {
        return false;
    }

    registers[SI4703_POWERCFG] = SI4703_POWERCFG_DMUTE | SI4703_POWERCFG_RDSM |
                                 SI4703_POWERCFG_ENABLE;
    // RDS on, 50 us de-emphasis (Europe)
    registers[SI4703_SYSCONFIG1] |= SI4703_SYSCONFIG1_RDS | SI4703_SYSCONFIG1_DE;
    // Seek threshold 0x19, 87.5 - 108 MHz band, 100 kHz spacing, volume 0
    registers[SI4703_SYSCONFIG2] = (0x19 << 8) | SI4703_SYSCONFIG2_SPACE_100KHZ;
    // Seek SNR threshold 4, FM impulse detection threshold 8
    registers[SI4703_SYSCONFIG3] = (registers[SI4703_SYSCONFIG3] &
                                    ~(SI4703_SYSCONFIG3_SKSNR | SI4703_SYSCONFIG3_SKCNT)) |
                                   (0x4 << 4) | 0x8;
    if(!si4703_write_registers(registers)) {
        return false;
    }
    furi_delay_ms(110);

    return si4703_read_registers(registers);
}

bool si4703_set_mute(uint16_t* registers, bool mute) {
    if(mute) {
        registers[SI4703_POWERCFG] &= ~SI4703_POWERCFG_DMUTE;
    } else {
        registers[SI4703_POWERCFG] |= SI4703_POWERCFG_DMUTE;
    }
    return si4703_write_registers(registers);
}

bool si4703_set_mono(uint16_t* registers, bool mono) {
    if(mono) {
        registers[SI4703_POWERCFG] |= SI4703_POWERCFG_MONO;
    } else {
        registers[SI4703_POWERCFG] &= ~SI4703_POWERCFG_MONO;
    }
    return si4703_write_registers(registers);
}

bool si4703_set_volume(uint16_t* registers, uint8_t volume) {
    registers[SI4703_SYSCONFIG2] = (registers[SI4703_SYSCONFIG2] & ~SI4703_SYSCONFIG2_VOLUME) |
                                   (MIN(volume, SI4703_VOLUME_MAX) & SI4703_SYSCONFIG2_VOLUME);
    return si4703_write_registers(registers);
}

// Waits until the STC bit of the chip matches the state, up to 200 ms.
static bool si4703_wait_stc(uint16_t* registers, bool state) {
    for(uint8_t i = 0; i < 40; i++) {
        if(!si4703_read_registers(registers)) {
            return false;
        }
        if(((registers[SI4703_STATUSRSSI] & SI4703_STATUSRSSI_STC) != 0) == state) {
            return true;
        }
        furi_delay_ms(5);
    }
    return false;
}

bool si4703_set_frequency(uint16_t* registers, int value) {
    // Stop any search or tuning in progress, the chip clears STC once SEEK and TUNE are cleared.
    registers[SI4703_POWERCFG] &= ~SI4703_POWERCFG_SEEK;
    registers[SI4703_CHANNEL] &= ~SI4703_CHANNEL_TUNE;
    if(!si4703_write_registers(registers) || !si4703_wait_stc(registers, false)) {
        return false;
    }

    int channel = (value - SI4703_FREQ_MIN) / SI4703_FREQ_SPACING;
    registers[SI4703_CHANNEL] = SI4703_CHANNEL_TUNE | (channel & SI4703_CHANNEL_CHAN);
    if(!si4703_write_registers(registers) || !si4703_wait_stc(registers, true)) {
        return false;
    }

    registers[SI4703_CHANNEL] &= ~SI4703_CHANNEL_TUNE;
    return si4703_write_registers(registers) && si4703_wait_stc(registers, false);
}

bool si4703_seek(uint16_t* registers, bool up) {
    registers[SI4703_POWERCFG] &= ~SI4703_POWERCFG_SKMODE;
    if(up) {
        registers[SI4703_POWERCFG] |= SI4703_POWERCFG_SEEKUP;
    } else {
        registers[SI4703_POWERCFG] &= ~SI4703_POWERCFG_SEEKUP;
    }
    registers[SI4703_POWERCFG] |= SI4703_POWERCFG_SEEK;
    return si4703_write_registers(registers);
}

bool si4703_get_seek_status(uint16_t* registers, bool* done, bool* failed, int* value) {
    if(!si4703_read_registers(registers)) {
        return false;
    }

    *done = (registers[SI4703_STATUSRSSI] & SI4703_STATUSRSSI_STC) != 0;
    *failed = (registers[SI4703_STATUSRSSI] & SI4703_STATUSRSSI_SFBL) != 0;
    *value = SI4703_FREQ_MIN +
             (registers[SI4703_READCHAN] & SI4703_READCHAN_CHAN) * SI4703_FREQ_SPACING;
    return true;
}

bool si4703_get_status(uint16_t* registers, uint8_t* rssi, bool* stereo) {
    if(!si4703_read_registers(registers)) {
        return false;
    }

    *rssi = registers[SI4703_STATUSRSSI] & SI4703_STATUSRSSI_RSSI;
    *stereo = (registers[SI4703_STATUSRSSI] & SI4703_STATUSRSSI_ST) != 0;
    return true;
}

bool si4703_get_rds(uint16_t* registers, uint16_t* blocks, uint8_t* errors) {
    if(!(registers[SI4703_STATUSRSSI] & SI4703_STATUSRSSI_RDSR)) {
        return false;
    }

    for(uint8_t i = 0; i < 4; i++) {
        blocks[i] = registers[SI4703_RDSA + i];
    }
    errors[0] = (registers[SI4703_STATUSRSSI] & SI4703_STATUSRSSI_BLERA) >> 9;
    errors[1] = (registers[SI4703_READCHAN] & SI4703_READCHAN_BLERB) >> 14;
    errors[2] = (registers[SI4703_READCHAN] & SI4703_READCHAN_BLERC) >> 12;
    errors[3] = (registers[SI4703_READCHAN] & SI4703_READCHAN_BLERD) >> 10;
    return true;
}
