#pragma once
// event_types.h — Bytebeat Machine V1.14 stage3
#include <cstdint>

enum EventType : uint8_t {
    EVT_PAD_TRIGGER,
    EVT_PARAM_CHANGE,
    EVT_DRUM_HIT,
    EVT_ROLL_ON,
    EVT_ROLL_OFF,
    EVT_FX_ON,
    EVT_FX_OFF,
    EVT_MUTATE,
    EVT_AFTERTOUCH,
    EVT_CLOCK_TICK,
    EVT_NOTE_ON,
    EVT_NOTE_OFF,
    EVT_DRUM_PARAM,
};

enum DrumId : uint8_t { DRUM_KICK = 0, DRUM_SNARE = 1, DRUM_HAT = 2 };
enum FxId : uint8_t {
    FX_STUTTER = 0,
    FX_FREEZE,
    FX_OCT_DOWN,
    FX_OCT_UP,
    FX_VIBRATO,
    FX_REPEAT_4,
    FX_REPEAT_8,
    FX_REPEAT_16
};

// Targets especiales para EVT_AFTERTOUCH (ev.target):
//   Nota MIDI (0–127)       → Note Mode velocity en tiempo real
//   PAD_MUTE (5)            → grain freeze wet (palma sobre MUTE)
//   AT_TARGET_REVERB_SNAP   → reverb wet momentáneo (SNAP pads)
static constexpr uint8_t AT_TARGET_REVERB_SNAP = 0xFD;

enum DrumParam : uint8_t {
    DRUM_PARAM_COLOR = 0,
    DRUM_PARAM_DECAY = 1,
    DRUM_PARAM_DUCK  = 2
};

enum ParamId : uint8_t {
    PARAM_MACRO = 0,
    PARAM_TONAL,
    PARAM_SPREAD,
    PARAM_DRIVE,
    PARAM_TIME_DIV,
    PARAM_SNAP_GATE,

    PARAM_GLIDE,
    PARAM_ENV_ATTACK,
    PARAM_ENV_RELEASE,
    PARAM_STUTTER_RATE,
    PARAM_GRAIN,
    PARAM_HP,

    PARAM_REVERB_ROOM,
    PARAM_REVERB_WET,
    PARAM_CHORUS,
    PARAM_DRUM_DECAY,
    PARAM_DRUM_COLOR,
    PARAM_DUCK_AMOUNT,
};

static constexpr uint32_t ROLL_THRESHOLD_MS = 120;

struct SequencerEvent {
    uint32_t  tick;
    EventType type;
    uint8_t   target;
    float     value;
};
