#pragma once
#include <stdint.h>

enum class EncoderMode : uint8_t {
    BPM = 0,
    SWING,
    ROOT,
    SCALE,
    MUTATE,
    COUNT
};

struct EncoderState {
    EncoderMode mode = EncoderMode::BPM;
    uint16_t bpm = 120;
    float swing_amount = 0.0f;
    uint8_t root = 0;
    uint8_t scale_id = 1;
    float mutate_amount = 0.5f;
};
