[![FlipC.org](https://flipc.org/victormico/flipperzero-radio/badge?nowerr=1)](https://flipc.org/victormico/flipperzero-radio?nowerr=1)

# flipperzero-radio
Application for flipperzero device to control radio boards.

## Supported boards
* [TEA5767](https://www.sparkfun.com/datasheets/Wireless/General/TEA5767.pdf)

## PIN
 - 3V3 (3V3, pin 9)  = VCC
 - GND (GND, pin 18) = GND
 - SCL (C0, pin 16)  = SCL
 - SDA (C1, pin 15)  = SDA

## Usage
When the app starts it looks for the radio board on the I2C bus and shows the wiring above until it is found.
Once connected it tunes the last station and shows the frequency, a tuning scale with the saved presets,
the signal level and whether the station is received in stereo.

| Button       | Short press                | Long press                 |
|--------------|----------------------------|----------------------------|
| Left / Right | Tune -/+ 0.1 MHz           | Seek previous/next station |
| Up / Down    | Next/previous preset       | Up: toggle forced mono     |
| OK           | Mute / unmute              | Save / remove preset       |
| Back         | Exit (radio goes to standby) |                          |

The last station, the presets (up to 16) and the mono setting are saved to `apps_data/radio` on the SD card.

## Build
```
ufbt
ufbt launch
```

## References
App inspired by [Radio](https://github.com/mathertel/Radio) project for Arduino

## Aknowledgements
Special thanks to [Derek Jamison](https://github.com/jamisonderek) for his amazing [Flipper Zero Tutorials](https://github.com/jamisonderek/flipper-zero-tutorials) that helped me to get started with Flipper Zero development.

And to [Oleksii Kutuzov](https://github.com/oleksiikutuzov) for his [Lightmeter](https://github.com/oleksiikutuzov/flipperzero-lightmeter) app, which was used as a template for this app.
