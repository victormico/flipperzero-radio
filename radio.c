#include <furi.h>
#include <furi_hal.h>

#include <gui/gui.h>
#include <storage/storage.h>
#include <toolbox/saved_struct.h>
#include <TEA5767.h>

#include "radio_icons.h"

/// The station tuned the first time the app is started is 95.30 MHz.
#define DEFAULT_STATION 9530

/// Manual tuning step, 100 kHz (frequencies are in 10 kHz units).
#define FREQUENCY_STEP 10

#define PRESETS_MAX 16

#define SETTINGS_PATH APP_DATA_PATH("radio.settings")
#define SETTINGS_MAGIC 0x52
#define SETTINGS_VERSION 1

/// Timer period. The seek status is polled on every tick.
#define TICK_MS 100
/// Signal level and device presence are polled every few ticks.
#define POLL_TICKS 5
/// Give up a station search after 5 seconds.
#define SEEK_TIMEOUT_TICKS 50
/// Status messages are shown for 1.5 seconds.
#define MESSAGE_TICKS 15

typedef enum {
    RadioStateNotFound,
    RadioStateTuned,
    RadioStateSeeking,
} RadioState;

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
    uint8_t preset_count;
    uint16_t presets[PRESETS_MAX]; // Sorted ascending.
} RadioSettings;

typedef struct {
    FuriString* buffer;
    RadioState state;
    uint8_t registers[5];
    RadioSettings settings;
    bool muted;
    struct RADIO_INFO info;
    // Station search
    bool seek_up;
    bool seek_wrapped;
    uint16_t seek_origin;
    uint32_t seek_ticks;
    // Transient status line message
    const char* message;
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
    return 4 + (frequency - TEA5767_FREQ_MIN) * 119 / (TEA5767_FREQ_MAX - TEA5767_FREQ_MIN);
}

static void render_not_found(Canvas* canvas) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 1, AlignCenter, AlignTop, "Connect TEA5767");
    canvas_draw_icon(canvas, 2, 20, &I_radio);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 42, 23, "SDA -> C1 (15)");
    canvas_draw_str(canvas, 42, 33, "SCL -> C0 (16)");
    canvas_draw_str(canvas, 42, 43, "VCC -> 5V (1)");
    canvas_draw_str(canvas, 42, 53, "GND -> GND (18)");
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, "Searching for device...");
}

static void render_signal(Canvas* canvas, uint8_t rssi) {
    // The chip reports the level from 0 to 15, shown as 0 to 5 bars.
    uint8_t bars = (rssi + 2) / 3;
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

static void render_scale(Canvas* canvas, RadioSettings* settings, uint16_t frequency) {
    canvas_draw_line(canvas, 4, 55, 123, 55);
    for(uint16_t mhz = 88; mhz <= 108; mhz++) {
        int32_t x = scale_x(mhz * 100);
        canvas_draw_line(canvas, x, (mhz % 5 == 0) ? 51 : 53, x, 54);
    }

    for(uint8_t i = 0; i < settings->preset_count; i++) {
        canvas_draw_box(canvas, scale_x(settings->presets[i]) - 1, 58, 3, 2);
    }

    // Needle
    int32_t x = scale_x(frequency);
    canvas_draw_line(canvas, x, 47, x, 61);
    canvas_draw_line(canvas, x - 1, 47, x + 1, 47);
}

static void render_radio(Canvas* canvas, RadioData* data) {
    RadioSettings* settings = &data->settings;
    uint16_t frequency = settings->frequency;

    // Status bar
    canvas_set_font(canvas, FontSecondary);
    if(data->state == RadioStateTuned) {
        canvas_draw_str(canvas, 0, 8, data->info.stereo ? "STEREO" : "MONO");
        render_signal(canvas, data->info.rssi);
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
    } else if(data->message) {
        canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignBottom, data->message);
    } else if(preset) {
        furi_string_printf(data->buffer, "Preset %d", preset);
        canvas_draw_str_aligned(
            canvas, 64, 42, AlignCenter, AlignBottom, furi_string_get_cstr(data->buffer));
    }

    render_scale(canvas, settings, frequency);
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
        render_not_found(canvas);
    } else {
        render_radio(canvas, data);
    }

    // Release the context, so other threads can update the data.
    furi_mutex_release(radio_context->mutex);
}

static void settings_load(RadioSettings* settings) {
    if(!saved_struct_load(
           SETTINGS_PATH, settings, sizeof(RadioSettings), SETTINGS_MAGIC, SETTINGS_VERSION) ||
       settings->frequency < TEA5767_FREQ_MIN || settings->frequency > TEA5767_FREQ_MAX ||
       settings->preset_count > PRESETS_MAX) {
        memset(settings, 0, sizeof(RadioSettings));
        settings->frequency = DEFAULT_STATION;
    }
}

static void settings_save(RadioSettings* settings) {
    saved_struct_save(
        SETTINGS_PATH, settings, sizeof(RadioSettings), SETTINGS_MAGIC, SETTINGS_VERSION);
}

static void show_message(RadioData* data, const char* message) {
    data->message = message;
    data->message_ticks = MESSAGE_TICKS;
}

static void tune(RadioData* data, uint16_t frequency) {
    data->settings.frequency = frequency;
    data->state = tea5767_set_frequency(data->registers, frequency) ? RadioStateTuned :
                                                                      RadioStateNotFound;
}

static void seek_start(RadioData* data, uint16_t frequency) {
    data->seek_ticks = 0;
    data->state = tea5767_seek(data->registers, frequency, data->seek_up) ? RadioStateSeeking :
                                                                            RadioStateNotFound;
}

static void seek(RadioData* data, bool up) {
    uint16_t frequency = data->settings.frequency;
    data->seek_up = up;
    data->seek_wrapped = false;
    data->seek_origin = frequency;

    // Start next to the current station so the search doesn't stop on it again.
    if(up && frequency + FREQUENCY_STEP <= TEA5767_FREQ_MAX) {
        seek_start(data, frequency + FREQUENCY_STEP);
    } else if(!up && frequency - FREQUENCY_STEP >= TEA5767_FREQ_MIN) {
        seek_start(data, frequency - FREQUENCY_STEP);
    } else {
        data->seek_wrapped = true;
        seek_start(data, up ? TEA5767_FREQ_MIN : TEA5767_FREQ_MAX);
    }
}

static void seek_update(RadioData* data) {
    bool done = false;
    bool band_limit = false;
    int frequency = 0;

    // Give the chip a full tick to start searching before reading its ready flag.
    if(++data->seek_ticks < 2) {
        return;
    }

    if(!tea5767_get_seek_status(&done, &band_limit, &frequency)) {
        data->state = RadioStateNotFound;
        return;
    }

    if(done && band_limit && !data->seek_wrapped) {
        // Reached the end of the band, continue from the other end.
        data->seek_wrapped = true;
        seek_start(data, data->seek_up ? TEA5767_FREQ_MIN : TEA5767_FREQ_MAX);
    } else if(done && !band_limit) {
        // Snap the station to the 100 kHz grid.
        frequency = (frequency + FREQUENCY_STEP / 2) / FREQUENCY_STEP * FREQUENCY_STEP;
        tune(data, CLAMP(frequency, TEA5767_FREQ_MAX, TEA5767_FREQ_MIN));
    } else if(done || data->seek_ticks > SEEK_TIMEOUT_TICKS) {
        tune(data, data->seek_origin);
        show_message(data, "No station found");
    } else if(frequency >= TEA5767_FREQ_MIN && frequency <= TEA5767_FREQ_MAX) {
        // Show the needle moving while searching.
        data->settings.frequency = frequency;
    }
}

static void step(RadioData* data, bool up) {
    int frequency = data->settings.frequency + (up ? FREQUENCY_STEP : -FREQUENCY_STEP);
    if(frequency > TEA5767_FREQ_MAX) {
        frequency = TEA5767_FREQ_MIN;
    } else if(frequency < TEA5767_FREQ_MIN) {
        frequency = TEA5767_FREQ_MAX;
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
    if(!tea5767_is_device_ready() || !tea5767_init(data->registers)) {
        return;
    }

    if(data->muted) {
        data->registers[REG_1] |= REG_1_MUTE;
    }
    if(data->settings.mono) {
        data->registers[REG_3] |= REG_3_MS;
    }
    tune(data, data->settings.frequency);
}

// Our main loop invokes this method after acquiring the mutex, so we can safely access the protected data.
static void radio_tick(RadioData* data) {
    data->ticks++;
    if(data->message_ticks > 0 && --data->message_ticks == 0) {
        data->message = NULL;
    }

    if(data->state == RadioStateSeeking) {
        seek_update(data);
    } else if(data->ticks % POLL_TICKS == 0) {
        if(data->state == RadioStateNotFound) {
            radio_connect(data);
        } else if(!tea5767_get_radio_info(data->registers, &data->info)) {
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
            next_preset(data, input->key == InputKeyUp);
            break;
        case InputKeyOk:
            data->muted = !data->muted;
            if(!tea5767_set_mute(data->registers, data->muted)) {
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
            data->settings.mono = !data->settings.mono;
            show_message(data, data->settings.mono ? "Forced mono" : "Stereo allowed");
            if(!tea5767_set_mono(data->registers, data->settings.mono)) {
                data->state = RadioStateNotFound;
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

    // Power the board from the 5V pin, most TEA5767 boards have a 5V regulator.
    bool otg_was_enabled = furi_hal_power_is_otg_enabled();
    if(!otg_was_enabled) {
        furi_hal_power_enable_otg();
    }

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
    if(data->state != RadioStateNotFound) {
        tea5767_set_standby(data->registers, true);
    }
    settings_save(&data->settings);
    if(!otg_was_enabled) {
        furi_hal_power_disable_otg();
    }

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
