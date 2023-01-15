#include <Carbon/Carbon.h>
#include <mach/mach_time.h>
#include <pthread.h>

#define HASHTABLE_IMPLEMENTATION
#include "misc/hashtable.h"
#undef HASHTABLE_IMPLEMENTATION
#include "misc/sbuffer.h"

#define EVENT_TAP_CALLBACK(name) CGEventRef name(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *reference)
typedef EVENT_TAP_CALLBACK(event_tap_callback);

//
//
// Struct definitions
//
//

struct event_tap
{
    CFMachPortRef handle;
    CFRunLoopSourceRef runloop_source;
};

struct input
{
    CGEventRef event;
    uint64_t timestamp;
};

struct recording
{
    struct input *events;
    uint64_t timestamp;
};

enum state
{
    STATE_IDLE     = 0,
    STATE_RECORD   = 1,
    STATE_PLAYBACK = 2
};

enum mod
{
    MOD_NONE  = 0x01,
    MOD_ALT   = 0x02,
    MOD_SHIFT = 0x04,
    MOD_CMD   = 0x08,
    MOD_CTRL  = 0x10,
    MOD_FN    = 0x20
};

//
//
// Global state
//
//

struct event_tap g_event_tap;
struct recording *g_active_recording;
struct table g_slot;
enum state g_state;

pthread_t g_playback_thread;
pid_t g_pid;

//
//
// Helpers
//
//

static TABLE_HASH_FUNC(hash_slot)
{
    return *(uint16_t *) key;
}

static TABLE_COMPARE_FUNC(compare_slot)
{
    return *(uint16_t *) key_a == *(uint16_t *) key_b;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static inline uint64_t get_wall_clock(void)
{
    uint64_t absolute = mach_absolute_time();
    Nanoseconds result = AbsoluteToNanoseconds(*(AbsoluteTime *) &absolute);
    return *(uint64_t *) &result;
}
#pragma clang diagnostic pop

static inline float get_seconds_elapsed(uint64_t start, uint64_t end)
{
    float result = ((float)(end - start) / 1000.0f) / 1000000.0f;
    return result;
}

static inline uint8_t mod_from_event(CGEventRef event)
{
    uint8_t mod = 0;
    CGEventFlags flags = CGEventGetFlags(event);

    if ((flags & kCGEventFlagMaskAlternate)   == kCGEventFlagMaskAlternate)   mod |= MOD_ALT;
    if ((flags & kCGEventFlagMaskShift)       == kCGEventFlagMaskShift)       mod |= MOD_SHIFT;
    if ((flags & kCGEventFlagMaskCommand)     == kCGEventFlagMaskCommand)     mod |= MOD_CMD;
    if ((flags & kCGEventFlagMaskControl)     == kCGEventFlagMaskControl)     mod |= MOD_CTRL;
    if ((flags & kCGEventFlagMaskSecondaryFn) == kCGEventFlagMaskSecondaryFn) mod |= MOD_FN;

    return mod;
}

//
//
// Core functionality
//
//

static void *playback_thread_proc(void *context)
{
    struct recording *recording = context;
    uint64_t last_timestamp = recording->timestamp;

    for (int i = 0; i < buf_len(recording->events); ++i) {
        if (g_state != STATE_PLAYBACK) return NULL;

        struct input *input = (struct input *) (recording->events + i);
        float wait_time = get_seconds_elapsed(last_timestamp, input->timestamp);

        while (wait_time > 0.0f) {
            uint64_t clock = get_wall_clock();
            usleep(wait_time * 700.0f);
            wait_time -= get_seconds_elapsed(clock, get_wall_clock());
        }

        CGEventPost(kCGHIDEventTap, input->event);
        last_timestamp = input->timestamp;
    }

    g_state = STATE_IDLE;
    return NULL;
}

static inline void record_input(CGEventRef event)
{
    buf_push(g_active_recording->events, ((struct input) {
        .event = (CGEventRef) CFRetain(event),
        .timestamp = CGEventGetTimestamp(event)
    }));
}

static inline void begin_recording(uint64_t timestamp)
{
    struct recording *recording = malloc(sizeof(struct recording));

    recording->events = NULL;
    recording->timestamp = timestamp;

    g_active_recording = recording;
    g_state = STATE_RECORD;
}

static inline void end_recording()
{
    g_state = STATE_IDLE;
    g_active_recording = NULL;
}

static inline void play_recording()
{
    g_state = STATE_PLAYBACK;
    struct recording *recording = NULL;
    pthread_create(&g_playback_thread, NULL, &playback_thread_proc, recording);
    pthread_detach(g_playback_thread);
}

static inline void stop_recording()
{
    g_state = STATE_IDLE;
    pthread_join(g_playback_thread, NULL);
}

static inline bool event_equals(CGEventRef event, uint8_t modifier, uint16_t keycode)
{
    uint8_t event_modifier = mod_from_event(event);
    uint16_t event_keycode = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
    return (event_modifier == modifier && event_keycode == keycode);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wswitch"
static EVENT_TAP_CALLBACK(key_handler)
{
    switch (type) {
    case kCGEventTapDisabledByTimeout:
    case kCGEventTapDisabledByUserInput: {
        struct event_tap *event_tap = (struct event_tap *) reference;
        if (event_tap->handle) CGEventTapEnable(event_tap->handle, true);
    } break;

    case kCGEventKeyDown: {
        pid_t source_pid = CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID);
        if (source_pid == g_pid) return event;

        switch (g_state) {
        case STATE_IDLE: {
            if (event_equals(event, MOD_CMD, kVK_ANSI_R)) {
                begin_recording(CGEventGetTimestamp(event));
            } else if (event_equals(event, MOD_CMD, kVK_ANSI_P)) {
                play_recording();
            }
        } break;

        case STATE_RECORD: {
            if (event_equals(event, MOD_CMD, kVK_ANSI_S)) {
                end_recording();
            } else {
                record_input(event);
            }
        } break;

        case STATE_PLAYBACK: {
            if (event_equals(event, MOD_CMD, kVK_ANSI_S)) {
                stop_recording();
            }
        } break;
        }
    } break;

    case kCGEventKeyUp: {
        pid_t source_pid = CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID);
        if (source_pid == g_pid) return event;

        case STATE_RECORD: {
            record_input(event);
        } break;
    } break;
    }

    return event;
}
#pragma clang diagnostic pop

bool event_tap_begin(struct event_tap *event_tap, uint32_t mask, event_tap_callback *callback)
{
    if (event_tap->handle) return true;

    event_tap->handle = CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault, mask, callback, event_tap);
    if (!event_tap->handle) return false;

    if (!CGEventTapIsEnabled(event_tap->handle)) {
        CFMachPortInvalidate(event_tap->handle);
        CFRelease(event_tap->handle);
        return false;
    }

    event_tap->runloop_source = CFMachPortCreateRunLoopSource(NULL, event_tap->handle, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), event_tap->runloop_source, kCFRunLoopCommonModes);

    return true;
}

void event_tap_end(struct event_tap *event_tap)
{
    if (!event_tap->handle) return;

    CGEventTapEnable(event_tap->handle, false);
    CFMachPortInvalidate(event_tap->handle);
    CFRunLoopRemoveSource(CFRunLoopGetMain(), event_tap->runloop_source, kCFRunLoopCommonModes);
    CFRelease(event_tap->runloop_source);
    CFRelease(event_tap->handle);
    event_tap->handle = NULL;
}

int main(int argc, char **argv)
{
    g_pid = getpid();

    if (event_tap_begin(&g_event_tap, (1 << kCGEventKeyDown) | (1 << kCGEventKeyUp), key_handler)) {
        table_init(&g_slot, 150, hash_slot, compare_slot);
        CGSetLocalEventsSuppressionInterval(0.0f);
        CGEnableEventStateCombining(false);
        CFRunLoopRun();
    }

    return EXIT_FAILURE;
}
