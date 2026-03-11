// dsp_chain.cpp — Bytebeat Machine V1.14
#include "dsp_chain.h"

void DspChain::init() {
    reset();
    reverb_.init();
    stutter_.init();
    chorus_.init();
    hpf_.init();
    grain_.init();
    snap_.init();
}

void DspChain::reset() {
    dc_l_.reset();   dc_r_.reset();
    lim_lr_.reset();
    hpf_.reset();
}

void DspChain::process(int16_t& left, int16_t& right) {
    // 1. Stutter (sobre mix crudo, antes de DC block)
    stutter_.process(left, right);

    // 2. DC block
    left  = dc_l_.process(left);
    right = dc_r_.process(right);

    // 3. HP filter momentáneo (SHIFT+POT5 / MIDI CC74)
    if (hpf_.is_active()) hpf_.process(left, right);

    // 4. Snap Gate (base layer POT5) — antes del clip para que el gate
    //    actúe sobre la señal completa y no sobre el saturado
    if (snap_.is_active()) snap_.process(left, right);

    // 5. Soft clip
    left  = clip_l_.process(left);
    right = clip_r_.process(right);

    // 6. Chorus (SHIFT+REC+POT2 / MIDI CC93) — después del clip, sobre señal limpia
    if (chorus_.is_active()) chorus_.process(left, right);

    // 7. Grain Freeze (SHIFT+POT4) — después del chorus
    if (grain_.is_active()) grain_.process(left, right);

    // 8. Reverb
    if (reverb_.is_active()) {
        int16_t mono_in = (int16_t)(((int32_t)left + right) >> 1);
        reverb_.process(mono_in, left, right);
    }

    // 9. Limiter stereo-linked
    lim_lr_.process_stereo(left, right);
}
