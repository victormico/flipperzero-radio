#include <furi.h>
#include <furi_hal.h>

#include <gui/gui.h>
#include <storage/storage.h>
#include <toolbox/saved_struct.h>
#include <TEA5767.h>
#include <Si4703.h>
#include <RDS.h>

#include "radio_icons.h"

/// The station tuned the first time the app is started is 95.30 MHz.
#define DEFAULT_STATION 9530

/// Manual tuning step, 100 kHz (frequencies are in 10 kHz units).
#define FREQUENCY_STEP 10

/// FM band supported by both chips, 87.50 - 108.00 MHz.
#define FREQUENCY_MIN 8750
#define FREQUENCY_MAX 10800

#define DEFAULT_VOLUME 8

#define PRESETS_MAX 16

#define SETTINGS_PATH APP_DATA_PATH("radio.settings")
#define SETTINGS_MAGIC 0x52
#define SETTINGS_VERSION 2

/// Timer period. The seek status and the Si4703 RDS data are polled on every tick.
#define TICK_MS 40
/// Signal level and device presence are polled every 500 ms.
#define POLL_TICKS (500 / TICK_MS)
/// Give the TEA5767 100 ms to start searching before reading its ready flag.
#define SEEK_START_TICKS (100 / TICK_MS + 1)
/// Give up a station search after 15 seconds.
#define SEEK_TIMEOUT_TICKS (15000 / TICK_MS)
/// Status messages are shown for 1.5 seconds.
#define MESSAGE_TICKS (1500 / TICK_MS)
/// The wiring of each chip is shown for 3 seconds while searching for the board.
#define WIRING_PAGE_TICKS (3000 / TICK_MS)

typedef enum {
    RadioStateNotFound,
    RadioStateTuned,
    RadioStateSeeking,
} RadioState;

typedef enum {
    RadioChipNone,
    RadioChipTea5767,
    RadioChipSi4703,
} RadioChip;

typedef enum {
    RadioEventTypeTick,
    RadioEventTypeKey,
} RadioEventType;

typedef struct {
    RadioEventType type; // The reason for this event.
    InputEvent input; // This data is specific to keypress data.
} RadioEvent;

// Persisted on the SD card between runs.
typedef struct {
    uint16_t frequency;
    bool mono;
    uint8_t volume; // Si4703 only.
    uint8_t preset_count;
    uint16_t presets[PRESETS_MAX]; // Sorted ascending.
} RadioSettings;

typedef struct {
    FuriString* buffer;
    RadioState state;
    RadioChip chip;
    uint8_t tea5767_registers[5];
    uint16_t si4703_registers[SI4703_REGISTERS];
    RadioSettings settings;
    bool muted;
    bool stereo;
    uint8_t signal_bars; // 0 to 5
    RdsData rds;
    // Station search
    bool seek_up;
    bool seek_wrapped;
    uint16_t seek_origin;
    uint32_t seek_ticks;
    // Transient status line message
    char message[32];
    uint32_t message_ticks;
    uint32_t ticks;
} RadioData;

typedef struct {
    FuriMessageQueue* queue; // Message queue (RadioEvent items to process).
    FuriMutex* mutex; // Used to provide thread safe access to data.
    RadioData* data; // Data accessed by multiple threads (acquire the mutex before accessing!)
} RadioContext;

// Invoked when input (button press) is detected.  We queue a message and then return to the caller.
static void input_callback(InputEvent* input_event, void* ctx) {
    furi_assert(ctx);
    FuriMessageQueue* queue = ctx;
    RadioEvent event = {.type = RadioEventTypeKey, .input = *input_event};
    furi_message_queue_put(queue, &event, FuriWaitForever);
}

// Invoked by the timer on every tick.  We queue a message and then return to the caller.
static void tick_callback(void* ctx) {
    furi_assert(ctx);
    FuriMessageQueue* queue = ctx;
    RadioEvent event = {.type = RadioEventTypeTick};
    // It's OK to loose this event if system overloaded (so we don't pass a wait value for 3rd parameter.)
    furi_message_queue_put(queue, &event, 0);
}

// Returns the 1-based preset number of the frequency, or 0 if it isn't a preset.
static uint8_t preset_number(RadioSettings* settings, uint16_t frequency) {
    for(uint8_t i = 0; i < settings->preset_count; i++) {
        if(settings->presets[i] == frequency) {
            return i + 1;
        }
    }
    return 0;
}

// Maps a frequency to an x coordinate of the tuning scale.
static int32_t scale_x(uint16_t frequency) {
    return 4 + (frequency - FREQUENCY_MIN) * 119 / (FREQUENCY_MAX - FREQUENCY_MIN);
}

// Shows the wiring of each supported chip in turn.
static void render_not_found(Canvas* canvas, uint32_t ticks) {
    bool si4703 = (ticks / WIRING_PAGE_TICKS) % 2;

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(
        canvas, 64, 1, AlignCenter, AlignTop, si4703 ? "Connect Si4703" : "Connect TEA5767");
    canvas_draw_icon(canvas, 2, 20, &I_radio);

    canvas_set_font(canvas, FontSecondary);
    if(si4703) {
        // Six wires, so there is no room for the searching line.
        canvas_draw_str(canvas, 42, 20, "SDIO -> C1 (15)");
        canvas_draw_str(canvas, 42, 28, "SCLK -> C0 (16)");
        canvas_draw_str(canvas, 42, 36, "RST -> A4 (4)");
        canvas_draw_str(canvas, 42, 44, "SEN -> 3V3 (9)");
        canvas_draw_str(canvas, 42, 52, "3.3V -> 3V3 (9)");
        canvas_draw_str(canvas, 42, 60, "GND -> GND (18)");
    } else {
        canvas_draw_str(canvas, 42, 23, "SDA -> C1 (15)");
        canvas_draw_str(canvas, 42, 33, "SCL -> C0 (16)");
        canvas_draw_str(canvas, 42, 43, "VCC -> 3V3 (9)");
        canvas_draw_str(canvas, 42, 53, "GND -> GND (18)");
        canvas_draw_str_aligned(
            canvas, 64, 63, AlignCenter, AlignBottom, "Searching for device...");
    }
}

static void render_signal(Canvas* canvas, uint8_t bars) {
    for(uint8_t i = 0; i < 5; i++) {
        int32_t x = 108 + i * 4;
        uint8_t height = 2 + i * 2;
        if(i < bars) {
            canvas_draw_box(canvas, x, 10 - height, 3, height);
        } else {
            canvas_draw_line(canvas, x, 9, x + 2, 9);
        }
    }
}

// The compact scale leaves room for the RDS radio text at the bottom.
static void render_scale(
    Canvas* canvas,
    RadioSettings* settings,
    uint16_t frequency,
    bool compact) {
    int32_t y = compact ? 51 : 55;
    canvas_draw_line(canvas, 4, y, 123, y);
    for(uint16_t mhz = 88; mhz <= 108; mhz++) {
        int32_t x = scale_x(mhz * 100);
        canvas_draw_line(canvas, x, y - ((mhz % 5 == 0) ? 4 : 2), x, y - 1);
    }

    for(uint8_t i = 0; i < settings->preset_count; i++) {
        canvas_draw_box(canvas, scale_x(settings->presets[i]) - 1, y + (compact ? 2 : 3), 3, 2);
    }

    // Needle
    int32_t x = scale_x(frequency);
    int32_t top = y - (compact ? 6 : 8);
    canvas_draw_line(canvas, x, top, x, y + (compact ? 3 : 6));
    canvas_draw_line(canvas, x - 1, top, x + 1, top);
}

// Scrolls the text when it doesn't fit the screen.
static void render_text(Canvas* canvas, const char* text, uint32_t ticks) {
    uint16_t width = canvas_string_width(canvas, text);
    if(width <= 128) {
        canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, text);
    } else {
        canvas_draw_str(canvas, 128 - ticks % (width + 128), 63, text);
    }
}

static void render_radio(Canvas* canvas, RadioData* data) {
    RadioSettings* settings = &data->settings;
    uint16_t frequency = settings->frequency;

    // Status bar
    canvas_set_font(canvas, FontSecondary);
    if(data->state == RadioStateTuned) {
        canvas_draw_str(canvas, 0, 8, data->stereo ? "STEREO" : "MONO");
        render_signal(canvas, data->signal_bars);
    }
    if(data->chip == RadioChipSi4703) {
        furi_string_printf(data->buffer, "Vol %d", settings->volume);
        canvas_draw_str_aligned(
            canvas, 104, 8, AlignRight, AlignBottom, furi_string_get_cstr(data->buffer));
    }
    if(data->muted) {
        uint16_t width = canvas_string_width(canvas, "MUTE") + 4;
        canvas_draw_box(canvas, 64 - width / 2, 0, width, 10);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str_aligned(canvas, 64, 1, AlignCenter, AlignTop, "MUTE");
        canvas_set_color(canvas, ColorBlack);
    }

    // Frequency
    furi_string_printf(data->buffer, "%d.%d", frequency / 100, (frequency % 100) / 10);
    canvas_set_font(canvas, FontBigNumbers);
    uint16_t number_width = canvas_string_width(canvas, furi_string_get_cstr(data->buffer));
    canvas_set_font(canvas, FontSecondary);
    uint16_t unit_width = canvas_string_width(canvas, "MHz");
    int32_t x = 64 - (number_width + 2 + unit_width) / 2;
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str(canvas, x, 31, furi_string_get_cstr(data->buffer));
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, x + number_width + 2, 31, "MHz");

    // Status line
    uint8_t preset = preset_number(settings, frequency);
    if(data->state == RadioStateSeeking) {
        canvas_draw_str_aligned(
            canvas, 64, 42, AlignCenter, AlignBottom, data->seek_up ? "Seeking >>" : "<< Seeking");
    } else if(data->message_ticks > 0) {
        canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignBottom, data->message);
    } else if(data->rds.ps[0]) {
        // Station name from RDS
        if(preset) {
            furi_string_printf(data->buffer, "P%d", preset);
            canvas_draw_str(canvas, 0, 42, furi_string_get_cstr(data->buffer));
        }
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignBottom, data->rds.ps);
        canvas_set_font(canvas, FontSecondary);
    } else if(preset) {
        furi_string_printf(data->buffer, "Preset %d", preset);
        canvas_draw_str_aligned(
            canvas, 64, 42, AlignCenter, AlignBottom, furi_string_get_cstr(data->buffer));
    }

    bool has_text = data->state == RadioStateTuned && data->rds.text[0];
    render_scale(canvas, settings, frequency, has_text);
    if(has_text) {
        render_text(canvas, data->rds.text, data->ticks);
    }
}

// Invoked by the draw callback to render the screen. We render our UI on the callback thread.
static void render_callback(Canvas* canvas, void* ctx) {
    // Attempt to aquire context, so we can read the data.
    RadioContext* radio_context = ctx;
    if(furi_mutex_acquire(radio_context->mutex, 200) != FuriStatusOk) {
        return;
    }

    RadioData* data = radio_context->data;
    if(data->state == RadioStateNotFound) {
        render_not_found(canvas, data->ticks);
    } else {
        render_radio(canvas, data);
    }

    // Release the context, so other threads can update the data.
    furi_mutex_release(radio_context->mutex);
}

static void settings_load(RadioSettings* settings) {
    if(!saved_struct_load(
           SETTINGS_PATH, settings, sizeof(RadioSettings), SETTINGS_MAGIC, SETTINGS_VERSION) ||
       settings->frequency < FREQUENCY_MIN || settings->frequency > FREQUENCY_MAX ||
       settings->preset_count > PRESETS_MAX || settings->volume > SI4703_VOLUME_MAX) {
        memset(settings, 0, sizeof(RadioSettings));
        settings->frequency = DEFAULT_STATION;
        settings->volume = DEFAULT_VOLUME;
    }
}

static void settings_save(RadioSettings* settings) {
    saved_struct_save(
        SETTINGS_PATH, settings, sizeof(RadioSettings), SETTINGS_MAGIC, SETTINGS_VERSION);
}

static void show_message(RadioData* data, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(data->message, sizeof(data->message), format, args);
    va_end(args);
    data->message_ticks = MESSAGE_TICKS;
}

// The chip_* functions hide the differences between the supported chips.

static bool chip_tune(RadioData* data, uint16_t frequency) {
    if(data->chip == RadioChipSi4703) {
        return si4703_set_frequency(data->si4703_registers, frequency);
    }
    return tea5767_set_frequency(data->tea5767_registers, frequency);
}

// The TEA5767 searches from the given frequency, the Si4703 from the tuned one.
static bool chip_seek(RadioData* data, uint16_t frequency) {
    if(data->chip == RadioChipSi4703) {
        return si4703_seek(data->si4703_registers, data->seek_up);
    }
    return tea5767_seek(data->tea5767_registers, frequency, data->seek_up);
}

static bool chip_get_seek_status(RadioData* data, bool* done, bool* band_limit, int* frequency) {
    if(data->chip == RadioChipSi4703) {
        return si4703_get_seek_status(data->si4703_registers, done, band_limit, frequency);
    }
    return tea5767_get_seek_status(done, band_limit, frequency);
}

static bool chip_set_mute(RadioData* data, bool mute) {
    if(data->chip == RadioChipSi4703) {
        return si4703_set_mute(data->si4703_registers, mute);
    }
    return tea5767_set_mute(data->tea5767_registers, mute);
}

static bool chip_set_mono(RadioData* data, bool mono) {
    if(data->chip == RadioChipSi4703) {
        return si4703_set_mono(data->si4703_registers, mono);
    }
    return tea5767_set_mono(data->tea5767_registers, mono);
}

// Reads the signal level, stereo indicator and RDS data.
static bool chip_update_status(RadioData* data) {
    if(data->chip == RadioChipSi4703) {
        uint8_t rssi = 0;
        uint16_t blocks[4];
        uint8_t errors[4];
        if(!si4703_get_status(data->si4703_registers, &rssi, &data->stereo)) {
            return false;
        }
        // The level is reported in dBµV, strong stations are above 50.
        data->signal_bars = MIN(rssi / 10, 5);
        if(si4703_get_rds(data->si4703_registers, blocks, errors)) {
            rds_process(&data->rds, blocks, errors);
        }
        return true;
    }

    struct RADIO_INFO info;
    if(!tea5767_get_radio_info(data->tea5767_registers, &info)) {
        return false;
    }
    // The level is reported from 0 to 15.
    data->signal_bars = (info.rssi + 2) / 3;
    data->stereo = info.stereo;
    return true;
}

static void chip_stop(RadioData* data) {
    if(data->chip == RadioChipSi4703) {
        // Also releases the reset pin, so do it even if the chip is gone.
        si4703_deinit(data->si4703_registers);
    } else if(data->chip == RadioChipTea5767 && data->state != RadioStateNotFound) {
        tea5767_set_standby(data->tea5767_registers, true);
    }
}

static void tune(RadioData* data, uint16_t frequency) {
    data->settings.frequency = frequency;
    rds_reset(&data->rds);
    data->state = chip_tune(data, frequency) ? RadioStateTuned : RadioStateNotFound;
}

static void seek_start(RadioData* data, uint16_t frequency) {
    data->seek_ticks = 0;
    rds_reset(&data->rds);
    data->state = chip_seek(data, frequency) ? RadioStateSeeking : RadioStateNotFound;
}

static void seek(RadioData* data, bool up) {
    uint16_t frequency = data->settings.frequency;
    data->seek_up = up;
    data->seek_wrapped = false;
    data->seek_origin = frequency;

    // Start next to the current station so the search doesn't stop on it again.
    if(data->chip == RadioChipSi4703) {
        // The Si4703 skips the tuned station and wraps around the band by itself.
        seek_start(data, frequency);
    } else if(up && frequency + FREQUENCY_STEP <= FREQUENCY_MAX) {
        seek_start(data, frequency + FREQUENCY_STEP);
    } else if(!up && frequency - FREQUENCY_STEP >= FREQUENCY_MIN) {
        seek_start(data, frequency - FREQUENCY_STEP);
    } else {
        data->seek_wrapped = true;
        seek_start(data, up ? FREQUENCY_MIN : FREQUENCY_MAX);
    }
}

static void seek_update(RadioData* data) {
    bool done = false;
    bool band_limit = false;
    int frequency = 0;

    if(++data->seek_ticks < SEEK_START_TICKS) {
        return;
    }

    if(!chip_get_seek_status(data, &done, &band_limit, &frequency)) {
        data->state = RadioStateNotFound;
        return;
    }

    if(done && band_limit && data->chip == RadioChipTea5767 && !data->seek_wrapped) {
        // Reached the end of the band, continue from the other end.
        data->seek_wrapped = true;
        seek_start(data, data->seek_up ? FREQUENCY_MIN : FREQUENCY_MAX);
    } else if(done && !band_limit) {
        // Snap the station to the 100 kHz grid.
        frequency = (frequency + FREQUENCY_STEP / 2) / FREQUENCY_STEP * FREQUENCY_STEP;
        tune(data, CLAMP(frequency, FREQUENCY_MAX, FREQUENCY_MIN));
    } else if(done || data->seek_ticks > SEEK_TIMEOUT_TICKS) {
        tune(data, data->seek_origin);
        show_message(data, "No station found");
    } else if(frequency >= FREQUENCY_MIN && frequency <= FREQUENCY_MAX) {
        // Show the needle moving while searching.
        data->settings.frequency = frequency;
    }
}

static void step(RadioData* data, bool up) {
    int frequency = data->settings.frequency + (up ? FREQUENCY_STEP : -FREQUENCY_STEP);
    if(frequency > FREQUENCY_MAX) {
        frequency = FREQUENCY_MIN;
    } else if(frequency < FREQUENCY_MIN) {
        frequency = FREQUENCY_MAX;
    }
    tune(data, frequency);
}

static void next_preset(RadioData* data, bool up) {
    RadioSettings* settings = &data->settings;
    if(settings->preset_count == 0) {
        show_message(data, "Hold OK to save a preset");
        return;
    }

    // Presets are sorted, so pick the closest one in the requested direction and wrap around.
    uint16_t frequency = up ? settings->presets[0] : settings->presets[settings->preset_count - 1];
    for(uint8_t i = 0; i < settings->preset_count; i++) {
        uint16_t preset = up ? settings->presets[i] :
                               settings->presets[settings->preset_count - 1 - i];
        if(up ? preset > settings->frequency : preset < settings->frequency) {
            frequency = preset;
            break;
        }
    }
    tune(data, frequency);
}

static void change_volume(RadioData* data, bool up) {
    RadioSettings* settings = &data->settings;
    if(up && settings->volume < SI4703_VOLUME_MAX) {
        settings->volume++;
    } else if(!up && settings->volume > 0) {
        settings->volume--;
    }
    show_message(data, "Volume %d/%d", settings->volume, SI4703_VOLUME_MAX);
    if(!si4703_set_volume(data->si4703_registers, settings->volume)) {
        data->state = RadioStateNotFound;
    }
}

static void toggle_preset(RadioData* data) {
    RadioSettings* settings = &data->settings;
    uint16_t frequency = settings->frequency;
    uint8_t preset = preset_number(settings, frequency);

    if(preset) {
        for(uint8_t i = preset; i < settings->preset_count; i++) {
            settings->presets[i - 1] = settings->presets[i];
        }
        settings->preset_count--;
        show_message(data, "Preset removed");
    } else if(settings->preset_count < PRESETS_MAX) {
        uint8_t i = settings->preset_count;
        while(i > 0 && settings->presets[i - 1] > frequency) {
            settings->presets[i] = settings->presets[i - 1];
            i--;
        }
        settings->presets[i] = frequency;
        settings->preset_count++;
        show_message(data, "Preset saved");
    } else {
        show_message(data, "Presets full");
        return;
    }
    settings_save(settings);
}

// Our main loop invokes this method after acquiring the mutex, so we can safely access the protected data.
static void radio_connect(RadioData* data) {
    if(tea5767_is_device_ready()) {
        if(!tea5767_init(data->tea5767_registers)) {
            return;
        }
        data->chip = RadioChipTea5767;
        if(data->muted) {
            data->tea5767_registers[REG_1] |= REG_1_MUTE;
        }
        if(data->settings.mono) {
            data->tea5767_registers[REG_3] |= REG_3_MS;
        }
    } else if(si4703_reset()) {
        if(!si4703_init(data->si4703_registers)) {
            return;
        }
        data->chip = RadioChipSi4703;
        if(!si4703_set_volume(data->si4703_registers, data->settings.volume) ||
           !si4703_set_mute(data->si4703_registers, data->muted) ||
           !si4703_set_mono(data->si4703_registers, data->settings.mono)) {
            return;
        }
    } else {
        return;
    }
    tune(data, data->settings.frequency);
}

// Our main loop invokes this method after acquiring the mutex, so we can safely access the protected data.
static void radio_tick(RadioData* data) {
    data->ticks++;
    if(data->message_ticks > 0) {
        data->message_ticks--;
    }

    if(data->state == RadioStateSeeking) {
        seek_update(data);
    } else if(data->state == RadioStateNotFound) {
        if(data->ticks % POLL_TICKS == 0) {
            radio_connect(data);
        }
    } else if(data->chip == RadioChipSi4703 || data->ticks % POLL_TICKS == 0) {
        // The Si4703 is read on every tick to catch all the RDS groups.
        if(!chip_update_status(data)) {
            data->state = RadioStateNotFound;
        }
    }
}

// Our main loop invokes this method after acquiring the mutex, so we can safely access the protected data.
static void radio_input(RadioData* data, InputEvent* input) {
    if(data->state == RadioStateNotFound) {
        return;
    }

    if(input->type == InputTypeShort) {
        switch(input->key) {
        case InputKeyLeft:
        case InputKeyRight:
            if(data->state == RadioStateSeeking) {
                tune(data, data->seek_origin);
            }
            step(data, input->key == InputKeyRight);
            break;
        case InputKeyUp:
        case InputKeyDown:
            if(data->chip == RadioChipSi4703) {
                change_volume(data, input->key == InputKeyUp);
            } else {
                next_preset(data, input->key == InputKeyUp);
            }
            break;
        case InputKeyOk:
            data->muted = !data->muted;
            if(!chip_set_mute(data, data->muted)) {
                data->state = RadioStateNotFound;
            }
            break;
        default:
            break;
        }
    } else if(input->type == InputTypeLong) {
        switch(input->key) {
        case InputKeyLeft:
        case InputKeyRight:
            if(data->state == RadioStateSeeking) {
                tune(data, data->seek_origin);
            }
            seek(data, input->key == InputKeyRight);
            break;
        case InputKeyUp:
        case InputKeyDown:
            if(data->chip == RadioChipSi4703) {
                // Up and down are used for the volume, so presets move to a long press.
                next_preset(data, input->key == InputKeyUp);
            } else if(input->key == InputKeyUp) {
                data->settings.mono = !data->settings.mono;
                show_message(data, data->settings.mono ? "Forced mono" : "Stereo allowed");
                if(!chip_set_mono(data, data->settings.mono)) {
                    data->state = RadioStateNotFound;
                }
            }
            break;
        case InputKeyOk:
            if(data->state == RadioStateTuned) {
                toggle_preset(data);
            }
            break;
        default:
            break;
        }
    }
}

int32_t radio_app(void* p) {
    UNUSED(p);

    // Configure our initial data.
    RadioContext* radio_context = malloc(sizeof(RadioContext));
    radio_context->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    radio_context->data = malloc(sizeof(RadioData));
    memset(radio_context->data, 0, sizeof(RadioData));
    radio_context->data->buffer = furi_string_alloc();
    radio_context->data->state = RadioStateNotFound;
    settings_load(&radio_context->data->settings);

    // Queue for events (tick or input)
    radio_context->queue = furi_message_queue_alloc(8, sizeof(RadioEvent));

    // Set ViewPort callbacks
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, render_callback, radio_context);
    view_port_input_callback_set(view_port, input_callback, radio_context->queue);

    // Open GUI and register view_port
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    // Try to find the radio straight away instead of waiting for the first poll.
    furi_mutex_acquire(radio_context->mutex, FuriWaitForever);
    radio_connect(radio_context->data);
    furi_mutex_release(radio_context->mutex);

    FuriTimer* timer =
        furi_timer_alloc(tick_callback, FuriTimerTypePeriodic, radio_context->queue);
    furi_timer_start(timer, furi_ms_to_ticks(TICK_MS));

    // Main loop
    RadioEvent event;
    bool processing = true;
    do {
        if(furi_message_queue_get(radio_context->queue, &event, FuriWaitForever) == FuriStatusOk) {
            furi_mutex_acquire(radio_context->mutex, FuriWaitForever);
            switch(event.type) {
            case RadioEventTypeKey:
                // Short press of back button exits the program.
                if(event.input.type == InputTypeShort && event.input.key == InputKeyBack) {
                    processing = false;
                } else {
                    radio_input(radio_context->data, &event.input);
                }
                break;
            case RadioEventTypeTick:
                radio_tick(radio_context->data);
                break;
            default:
                break;
            }
            furi_mutex_release(radio_context->mutex);

            // Send signal to update the screen (callback will get invoked at some point later.)
            view_port_update(view_port);
        } else {
            // We had an issue getting message from the queue, so exit application.
            processing = false;
        }
    } while(processing);

    // Stop the radio and remember the last station.
    RadioData* data = radio_context->data;
    if(data->state == RadioStateSeeking) {
        data->settings.frequency = data->seek_origin;
    }
    chip_stop(data);
    settings_save(&data->settings);

    // Free resources
    furi_timer_free(timer);
    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(radio_context->queue);
    furi_mutex_free(radio_context->mutex);
    furi_string_free(radio_context->data->buffer);
    free(radio_context->data);
    free(radio_context);

    return 0;
}
