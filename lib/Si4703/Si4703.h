/**
 * @file Si4703.h
 * @brief Implementation to control the Si4703 radio chip.
 * @version 0.1
 * @date 2026-10-25
 *
 * @copyright GPLv3
 *
 * Based on the Si4702/03 datasheet and the AN230 programming guide.
 */

#pragma once

#include <furi.h>
#include <furi_hal.h>
#include <stdint.h>

// ----- Definitions for the Wire communication

#define SI4703_ADR 0x20 // I2C address of Si4703 (0x10 shifted)

// Reset pin. The chip only starts in I2C mode when RST rises while SDIO is low.
#define SI4703_RST_PIN (&gpio_ext_pa4)
#define SI4703_SDIO_PIN (&gpio_ext_pc1)
#define SI4703_SCLK_PIN (&gpio_ext_pc0)

// ----- Radio chip specific definitions including the registers

#define SI4703_REGISTERS 16

#define SI4703_POWERCFG 0x02
#define SI4703_POWERCFG_DMUTE 0x4000
#define SI4703_POWERCFG_MONO 0x2000
#define SI4703_POWERCFG_RDSM 0x0800
#define SI4703_POWERCFG_SKMODE 0x0400
#define SI4703_POWERCFG_SEEKUP 0x0200
#define SI4703_POWERCFG_SEEK 0x0100
#define SI4703_POWERCFG_DISABLE 0x0040
#define SI4703_POWERCFG_ENABLE 0x0001

#define SI4703_CHANNEL 0x03
#define SI4703_CHANNEL_TUNE 0x8000
#define SI4703_CHANNEL_CHAN 0x03FF

#define SI4703_SYSCONFIG1 0x04
#define SI4703_SYSCONFIG1_RDS 0x1000
#define SI4703_SYSCONFIG1_DE 0x0800

#define SI4703_SYSCONFIG2 0x05
#define SI4703_SYSCONFIG2_SEEKTH 0xFF00
#define SI4703_SYSCONFIG2_BAND 0x00C0
#define SI4703_SYSCONFIG2_SPACE 0x0030
#define SI4703_SYSCONFIG2_SPACE_100KHZ 0x0010
#define SI4703_SYSCONFIG2_VOLUME 0x000F

#define SI4703_SYSCONFIG3 0x06
#define SI4703_SYSCONFIG3_SKSNR 0x00F0
#define SI4703_SYSCONFIG3_SKCNT 0x000F

#define SI4703_TEST1 0x07
#define SI4703_TEST1_XOSCEN 0x8000

#define SI4703_STATUSRSSI 0x0A
#define SI4703_STATUSRSSI_RDSR 0x8000
#define SI4703_STATUSRSSI_STC 0x4000
#define SI4703_STATUSRSSI_SFBL 0x2000
#define SI4703_STATUSRSSI_BLERA 0x0600
#define SI4703_STATUSRSSI_ST 0x0100
#define SI4703_STATUSRSSI_RSSI 0x00FF

#define SI4703_READCHAN 0x0B
#define SI4703_READCHAN_BLERB 0xC000
#define SI4703_READCHAN_BLERC 0x3000
#define SI4703_READCHAN_BLERD 0x0C00
#define SI4703_READCHAN_CHAN 0x03FF

#define SI4703_RDSA 0x0C
#define SI4703_RDSB 0x0D
#define SI4703_RDSC 0x0E
#define SI4703_RDSD 0x0F

// FM band limits in 10 kHz units (87.50 - 108.00 MHz), 100 kHz channel spacing
#define SI4703_FREQ_MIN 8750
#define SI4703_FREQ_MAX 10800
#define SI4703_FREQ_SPACING 10

#define SI4703_VOLUME_MAX 15

/**
 * @brief Pulse the reset pin to start the chip in I2C mode
 * @return True if the chip answers afterwards.
 */
bool si4703_reset(void);

/**
 * @brief Power down the chip and release the reset pin
 */
void si4703_deinit(uint16_t* registers);

bool si4703_is_device_ready(void);

/**
 * @brief Read all 16 registers into the shadow registers
 */
bool si4703_read_registers(uint16_t* registers);

/**
 * @brief Write the shadow registers 0x02 to 0x07 to the chip
 */
bool si4703_write_registers(uint16_t* registers);

/**
 * @brief Start the oscillator and power up the receiver (takes about 600 ms)
 */
bool si4703_init(uint16_t* registers);

bool si4703_set_mute(uint16_t* registers, bool mute);
bool si4703_set_mono(uint16_t* registers, bool mono);

/**
 * @brief Set the volume
 * @param volume from 0 (silent) to SI4703_VOLUME_MAX
 */
bool si4703_set_volume(uint16_t* registers, uint8_t volume);

/**
 * @brief Tune a frequency, stopping any search in progress
 * @param value frequency in 10 kHz units (9530 = 95.30 MHz)
 */
bool si4703_set_frequency(uint16_t* registers, int value);

/**
 * @brief Start the hardware station search from the tuned frequency. It wraps around at the band limits.
 * @param up search upwards when true, downwards otherwise
 */
bool si4703_seek(uint16_t* registers, bool up);

/**
 * @brief Check whether the search started with si4703_seek has finished
 * @param done true when the search has stopped
 * @param failed true when no station was found in the whole band
 * @param value frequency in 10 kHz units where the search is or stopped
 */
bool si4703_get_seek_status(uint16_t* registers, bool* done, bool* failed, int* value);

/**
 * @brief Read the status of the chip
 * @param rssi signal level in dBµV
 * @param stereo true when the station is received in stereo
 */
bool si4703_get_status(uint16_t* registers, uint8_t* rssi, bool* stereo);

/**
 * @brief Get the RDS group from the last read registers (see si4703_get_status)
 * @param blocks the 4 blocks of the RDS group
 * @param errors errors of each block: 0 none, 1 = 1-2 corrected, 2 = 3-5 corrected, 3 = uncorrectable
 * @return True if a group is ready.
 */
bool si4703_get_rds(uint16_t* registers, uint16_t* blocks, uint8_t* errors);
