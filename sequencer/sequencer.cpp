// sequencer.cpp — V1.5.0 hybrid sequence capture
#include "sequencer.h"
#include "hardware/sync.h"
#include "../utils/debug_log.h"
#include "pico/stdlib.h"
#include <cstring>

namespace {
inline float clampf_local(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

struct GrooveProfile {
    float timing_bias[8];
    float velocity_bias[8];
};

inline const GrooveProfile& groove_profile(GrooveTemplate tpl) {
    static const GrooveProfile kProfiles[] = {
        {{0.f,0.f,0.f,0.f,0.f,0.f,0.f,0.f}, {0.f,0.f,0.f,0.f,0.f,0.f,0.f,0.f}},
        {{0.f,0.38f,0.f,0.30f,0.f,0.42f,0.f,0.32f}, {0.05f,-0.04f,0.03f,-0.03f,0.06f,-0.05f,0.02f,-0.03f}},
        {{0.f,0.52f,0.f,0.48f,0.f,0.56f,0.f,0.50f}, {0.00f,-0.02f,0.03f,-0.03f,0.01f,-0.01f,0.04f,-0.04f}},
        {{0.f,0.44f,0.08f,0.35f,0.00f,0.50f,0.10f,0.28f}, {0.08f,-0.06f,0.04f,-0.05f,0.10f,-0.08f,0.03f,-0.04f}},
        {{0.f,0.62f,0.f,0.58f,0.f,0.64f,0.f,0.60f}, {0.02f,-0.02f,0.01f,-0.01f,0.02f,-0.02f,0.01f,-0.01f}},
    };
    return kProfiles[static_cast<uint8_t>(tpl) % static_cast<uint8_t>(GrooveTemplate::COUNT)];
}
}

void Sequencer::init() {
    set_bpm(120.0f);
    swing_ = 0.0f;
    clear_sequence();
    groove_template_ = GrooveTemplate::STRAIGHT;
}

void Sequencer::clear_step(SequenceStep& step) {
    step.snapshot_mask = 0;
    step.note_mask = 0;
    step.note_tie_mask = 0;
    step.drum_mask = 0;
    step.fx_hold_mask = 0;
    step.arp_enabled = false;
    step.param_mask = 0;
    std::memset(step.param_values, 0, sizeof(step.param_values));
    std::memset(step.note_midi, 0, sizeof(step.note_midi));
    step.chance_q8 = 255;
}

void Sequencer::clear_sequence() {
    for (auto& step : sequence_) clear_step(step);
    seq_length_ = 0;
    write_step_ = 0;
    play_step_ = 0;
    last_step_ = 0xFF;
    arp_play_slot_ = 0;
    active_fx_mask_ = 0;
    active_note_mask_ = 0;
    std::memset(active_note_midi_, 0, sizeof(active_note_midi_));
    sequence_valid_ = false;
    for (auto &p : pending_) p.active = false;
}

void Sequencer::normalize_sequence_length() {
    while (seq_length_ > 0 && step_is_empty(sequence_[seq_length_ - 1])) {
        --seq_length_;
    }
    sequence_valid_ = seq_length_ > 0;
    if (seq_length_ == 0) {
        play_step_ = 0;
        if (!manual_step_write_) write_step_ = 0;
    } else if (play_step_ >= seq_length_) {
        play_step_ = 0;
    }
}

bool Sequencer::step_is_empty(const SequenceStep& step) const {
    return step.snapshot_mask == 0 && step.note_mask == 0 && step.note_tie_mask == 0 && step.drum_mask == 0 && step.fx_hold_mask == 0 &&
           !step.arp_enabled && step.param_mask == 0 && step.chance_q8 == 255;
}

uint8_t Sequencer::quantize_param(float value) const {
    value = clampf_local(value, 0.0f, 1.0f);
    return (uint8_t)(value * 255.0f + 0.5f);
}

float Sequencer::dequantize_param(uint8_t value) const {
    return (float)value / 255.0f;
}

SequenceStep& Sequencer::current_record_step() {
    if (play_state_ == PlayState::RECORDING && !manual_step_write_ && has_sequence()) {
        uint8_t target = (last_step_ != 0xFF) ? last_step_ : play_step_;
        if (target >= seq_length_) target = 0;
        write_step_ = target;
    }
    if (write_step_ >= MAX_SEQ_STEPS) write_step_ = MAX_SEQ_STEPS - 1;
    return sequence_[write_step_];
}


bool Sequencer::step_has_snapshot(uint8_t step) const {
    return step < MAX_SEQ_STEPS && !step_is_empty(sequence_[step]) && (sequence_[step].snapshot_mask != 0);
}

bool Sequencer::step_has_note(uint8_t step) const {
    return step < MAX_SEQ_STEPS && !step_is_empty(sequence_[step]) && ((sequence_[step].note_mask | sequence_[step].note_tie_mask) != 0);
}

bool Sequencer::step_has_drum(uint8_t step) const {
    return step < MAX_SEQ_STEPS && !step_is_empty(sequence_[step]) && sequence_[step].drum_mask != 0;
}

bool Sequencer::step_has_motion(uint8_t step) const {
    if (step >= MAX_SEQ_STEPS || step_is_empty(sequence_[step])) return false;
    const SequenceStep& s = sequence_[step];
    return s.param_mask != 0 || s.fx_hold_mask != 0 || s.arp_enabled || s.chance_q8 < 255;
}

bool Sequencer::step_has_any(uint8_t step) const {
    return step < MAX_SEQ_STEPS && !step_is_empty(sequence_[step]);
}


float Sequencer::current_step_chance() const {
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return 1.0f;
    return (float)sequence_[idx].chance_q8 / 255.0f;
}

void Sequencer::adjust_current_step_chance(int delta) {
    if (!manual_step_write_ && !is_overdub() && !has_sequence()) return;
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return;
    int value = (int)sequence_[idx].chance_q8 + delta * 12;
    if (value < 13) value = 13;
    if (value > 255) value = 255;
    sequence_[idx].chance_q8 = (uint8_t)value;
    if ((idx + 1u) > seq_length_) seq_length_ = idx + 1u;
    sequence_valid_ = seq_length_ > 0;
    LOG_AUDIO("SEQ: step chance step=%u -> %u%%", (unsigned)idx, (unsigned)((value * 100 + 127) / 255));
}

void Sequencer::reset_current_step_chance() {
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return;
    sequence_[idx].chance_q8 = 255;
    LOG_AUDIO("SEQ: step chance reset step=%u", (unsigned)idx);
}
uint8_t Sequencer::visible_page_base() const {
    uint8_t anchor = 0;
    if (manual_step_write_) anchor = write_step_;
    else if (play_state_ == PlayState::PLAYING || play_state_ == PlayState::RECORDING) {
        anchor = (last_step_ != 0xFF) ? last_step_ : play_step_;
    }
    return (uint8_t)((anchor / 8u) * 8u);
}
uint8_t Sequencer::current_edit_step_index() const {
    if (manual_step_write_) return write_step_;
    if (play_state_ == PlayState::RECORDING && has_sequence()) {
        uint8_t target = (last_step_ != 0xFF) ? last_step_ : play_step_;
        return target < seq_length_ ? target : 0;
    }
    if (has_sequence()) {
        uint8_t target = (last_step_ != 0xFF) ? last_step_ : play_step_;
        return target < seq_length_ ? target : 0;
    }
    return 0;
}
const SequenceStep& Sequencer::current_play_step_ref() const {
    static SequenceStep empty;
    if (!has_sequence() || play_step_ >= seq_length_) return empty;
    return sequence_[play_step_];
}

void Sequencer::arm_record() {
    clear_sequence();
    manual_step_write_ = false;
    armed_record_ = true;
    preroll_steps_left_ = 16;
    tick_ = 0;
    play_step_ = 0;
    play_state_ = (clock_src_ == ClockSource::EXT) ? PlayState::ARMED : PlayState::PLAYING;
    last_tick_us_ = time_us_64();
    LOG_AUDIO("SEQ: armed record preroll=%u", (unsigned)preroll_steps_left_);
}

uint32_t Sequencer::rng_next() {
    random_state_ ^= (random_state_ << 13);
    random_state_ ^= (random_state_ >> 17);
    random_state_ ^= (random_state_ << 5);
    return random_state_;
}

float Sequencer::rand01() {
    return (float)(rng_next() & 0x00FFFFFFu) / 16777215.0f;
}

void Sequencer::mutate_generative_sequence(float amount, bool wild) {
    amount = clampf_local(amount, 0.0f, 1.0f);

    random_state_ ^= (uint32_t)(tick_ + 1u) * 0x9E3779B9u;
    random_state_ ^= wild ? 0xA5A55A5Au : 0x3C6EF372u;

    generative_enabled_ = true;
    generative_wild_    = wild;

    const float energy = wild ? (0.45f + amount * 0.40f)
                              : (0.18f + amount * 0.22f);

    chain_remaining_ = wild ? (uint8_t)(12 + (uint8_t)(amount * 18.0f))
                            : (uint8_t)(6 + (uint8_t)(amount * 8.0f));

    if (wild) {
        step_len_ticks_ = (amount > 0.82f) ? (INT_PPQN / 8)
                                           : (amount > 0.42f ? (INT_PPQN / 4) : (INT_PPQN / 2));
    } else {
        if (amount < 0.30f) {
            step_len_ticks_ = INT_PPQN / 2;
        } else if (amount < 0.78f) {
            step_len_ticks_ = INT_PPQN / 4;
        } else {
            step_len_ticks_ = INT_PPQN / 2;
        }
    }
    if (step_len_ticks_ == 0) step_len_ticks_ = INT_PPQN / 2;

    slot_span_         = (uint8_t)(1 + (wild ? (int)(amount * 3.0f) : (amount > 0.60f ? 1 : 0)));
    bars_per_phrase_   = wild ? (amount > 0.55f ? 1u : 2u)
                              : (amount < 0.35f ? 4u : (amount < 0.72f ? 2u : 1u));
    slot_jump_prob_    = wild ? (0.12f + amount * 0.36f) : (0.03f + amount * 0.10f);
    slot_trigger_prob_ = wild ? 1.00f : (0.90f + amount * 0.08f);
    retrig_prob_       = wild ? (0.05f + amount * 0.22f) : (0.01f + amount * 0.06f);

    kick_prob_         = wild ? (0.56f + energy * 0.22f) : (0.56f + energy * 0.12f);
    snare_prob_        = wild ? (0.18f + energy * 0.28f) : (0.10f + energy * 0.12f);
    hat_prob_          = wild ? (0.42f + energy * 0.36f) : (0.34f + energy * 0.16f);
    stutter_on_prob_   = wild ? (0.04f + amount * 0.20f) : (0.00f + amount * 0.04f);
    stutter_off_prob_  = wild ? (0.12f + (1.0f - amount) * 0.10f)
                              : (0.16f + (1.0f - amount) * 0.06f);

    const float slot_move_p = wild ? (0.20f + amount * 0.28f)
                                   : (0.05f + amount * 0.08f);
    if (rand01() < slot_move_p) {
        if (wild && rand01() < 0.35f) {
            current_slot_ = (uint8_t)(rng_next() % 8u);
        } else {
            int jump = 1 + (int)(rng_next() % (uint32_t)(slot_span_ > 0 ? slot_span_ : 1));
            if (rand01() < 0.5f) jump = -jump;
            int next = (int)current_slot_ + jump;
            while (next < 0) next += 8;
            while (next > 7) next -= 8;
            current_slot_ = (uint8_t)next;
        }
    }

    if (play_state_ == PlayState::STOPPED) play();

    LOG_AUDIO("SEQ: mutate sequence amount=%.2f mode=%s len=%u step=%lu slot=%u span=%u jump=%.2f retrig=%.2f",
              amount, wild ? "WILD" : "CTRL",
              (unsigned)chain_remaining_,
              (unsigned long)step_len_ticks_,
              (unsigned)current_slot_,
              (unsigned)slot_span_,
              slot_jump_prob_,
              retrig_prob_);
}

void Sequencer::start_random_chain(RandomizeMode mode) {
    const bool wild = (mode == RandomizeMode::WILD);
    mutate_generative_sequence(wild ? 0.85f : 0.45f, wild);
    LOG_AUDIO("SEQ: random chain %s len=%u start_slot=%u",
              wild ? "WILD" : "CTRL",
              (unsigned)chain_remaining_,
              (unsigned)current_slot_);
}

void Sequencer::run_generative_step(RingBuffer<SequencerEvent, 128>& queue) {
    if (!generative_enabled_ || has_sequence()) return;
    const uint32_t step_len = step_len_ticks_ ? step_len_ticks_ : (INT_PPQN / 2);
    const uint32_t bar_len  = INT_PPQN * 4;
    const uint32_t phrase_bars = bars_per_phrase_ ? bars_per_phrase_ : 1u;
    const uint32_t phrase_len = bar_len * phrase_bars;

    if ((tick_ % step_len) != 0) return;

    const bool phrase_edge = ((tick_ % phrase_len) == 0);
    if (phrase_edge || rand01() < slot_jump_prob_) {
        if (generative_wild_) {
            if (rand01() < 0.55f) {
                current_slot_ = (uint8_t)(rng_next() % 8u);
            } else {
                int jump = 1 + (int)(rng_next() % (uint32_t)(slot_span_ > 0 ? slot_span_ : 1));
                if (rand01() < 0.5f) jump = -jump;
                int next = (int)current_slot_ + jump;
                while (next < 0) next += 8;
                while (next > 7) next -= 8;
                current_slot_ = (uint8_t)next;
            }
        } else {
            int jump = 1 + (int)(rng_next() % (uint32_t)(slot_span_ > 0 ? slot_span_ : 1));
            if (rand01() < 0.5f) jump = -jump;
            int next = (int)current_slot_ + jump;
            while (next < 0) next += 8;
            while (next > 7) next -= 8;
            current_slot_ = (uint8_t)next;
        }
    }

    if (rand01() < slot_trigger_prob_) {
        enqueue_event(queue, {tick_, EVT_PAD_TRIGGER, current_slot_, 1.0f}, true);
        if (rand01() < retrig_prob_) {
            enqueue_event(queue, {tick_, EVT_PAD_TRIGGER, current_slot_, 0.72f}, true);
        }
    }

    const bool on_quarter = ((tick_ % INT_PPQN) == 0);
    const bool on_backbeat = ((tick_ % (INT_PPQN * 2)) == (INT_PPQN / 2));

    if (on_quarter || rand01() < kick_prob_ * 0.30f) {
        enqueue_event(queue, {tick_, EVT_DRUM_HIT, (uint8_t)DRUM_KICK, 1.0f}, true);
    }
    if (on_backbeat || rand01() < snare_prob_ * 0.22f) {
        enqueue_event(queue, {tick_, EVT_DRUM_HIT, (uint8_t)DRUM_SNARE, 1.0f}, true);
    }
    if (rand01() < hat_prob_) {
        enqueue_event(queue, {tick_, EVT_DRUM_HIT, (uint8_t)DRUM_HAT, generative_wild_ ? 0.90f : 0.78f}, true);
    }

    if (rand01() < stutter_on_prob_) {
        enqueue_event(queue, {tick_, EVT_FX_ON, (uint8_t)FX_STUTTER, 1.0f}, true);
    } else if (rand01() < stutter_off_prob_) {
        enqueue_event(queue, {tick_, EVT_FX_OFF, (uint8_t)FX_STUTTER, 0.0f}, true);
    }

    if (chain_remaining_ > 0) --chain_remaining_;
    if (chain_remaining_ == 0) {
        generative_enabled_ = false;
        LOG_AUDIO("SEQ: random chain finished");
    }
}

void Sequencer::set_bpm(float bpm) {
    bpm_            = bpm;
    tick_period_us_ = (uint32_t)(60000000.0f / (bpm_ * INT_PPQN));
}

void Sequencer::set_swing(float swing) {
    if (swing < 0.0f) swing = 0.0f;
    if (swing > 0.65f) swing = 0.65f;
    swing_ = swing;
}

void Sequencer::set_groove_template(GrooveTemplate tpl) {
    groove_template_ = static_cast<GrooveTemplate>(static_cast<uint8_t>(tpl) % static_cast<uint8_t>(GrooveTemplate::COUNT));
    LOG_AUDIO("SEQ: groove template=%u", (unsigned)static_cast<uint8_t>(groove_template_));
}

void Sequencer::adjust_groove_template(int delta) {
    if (delta == 0) return;
    int t = static_cast<int>(groove_template_) + (delta > 0 ? 1 : -1);
    const int count = static_cast<int>(GrooveTemplate::COUNT);
    while (t < 0) t += count;
    while (t >= count) t -= count;
    set_groove_template(static_cast<GrooveTemplate>(t));
}

uint32_t Sequencer::current_tick_interval_us(uint32_t tick_index) const {
    if (swing_ <= 0.0f || tick_period_us_ == 0) return tick_period_us_;

    constexpr uint32_t TICKS_PER_EIGHTH = INT_PPQN / 2;
    constexpr uint32_t TICKS_PER_QUARTER = INT_PPQN;

    const uint32_t quarter_pos = tick_index % TICKS_PER_QUARTER;
    const bool first_eighth = quarter_pos < TICKS_PER_EIGHTH;
    const float triplet_blend = swing_ / 3.0f;
    const float multiplier = first_eighth ? (1.0f + triplet_blend)
                                         : (1.0f - triplet_blend);

    const float interval_f = (float)tick_period_us_ * multiplier;
    return (uint32_t)(interval_f < 1.0f ? 1.0f : interval_f);
}

void Sequencer::set_clock_source(ClockSource src) {
    uint32_t s     = save_and_disable_interrupts();
    ticks_pending_ = 0;
    restore_interrupts(s);
    clock_src_ = src;
    LOG_AUDIO("SEQ: source → %s", src == ClockSource::EXT ? "EXT" : "INT");
}

void Sequencer::update_int(uint64_t now_us) {
    if (clock_src_ != ClockSource::INT) return;
    if (play_state_ == PlayState::STOPPED || play_state_ == PlayState::ARMED) return;

    while (true) {
        const uint32_t interval_us = current_tick_interval_us(tick_ + ticks_pending_);
        if (now_us - last_tick_us_ < interval_us) break;

        last_tick_us_ += interval_us;
        if (ticks_pending_ < MAX_TICKS) {
            ticks_pending_++;
        } else {
            break;
        }
    }
}

void Sequencer::on_ext_tick() {
    if (clock_src_ != ClockSource::EXT) return;

    if (play_state_ == PlayState::ARMED) {
        play_state_   = PlayState::PLAYING;
        tick_         = 0;
        last_tick_us_ = time_us_64();
        LOG_AUDIO("SEQ: PLAY — primer pulso EXT");
    }

    if (play_state_ == PlayState::PLAYING || play_state_ == PlayState::RECORDING) {
        uint32_t s  = save_and_disable_interrupts();
        uint16_t nx = ticks_pending_ + INT_MULT;
        ticks_pending_ = (nx > MAX_TICKS) ? MAX_TICKS : nx;
        restore_interrupts(s);
    }
}

void Sequencer::play() {
    manual_step_write_ = false;
    if (clock_src_ == ClockSource::EXT) {
        play_state_ = PlayState::ARMED;
        LOG_AUDIO("SEQ: ARMED");
    } else {
        play_state_   = PlayState::PLAYING;
        last_tick_us_ = time_us_64();
        tick_         = 0;
        play_step_    = 0;
        last_step_    = 0xFF;
        active_fx_mask_ = 0;
        active_note_mask_ = 0;
        std::memset(active_note_midi_, 0, sizeof(active_note_midi_));
        LOG_AUDIO("SEQ: PLAY INT @ %.1f BPM", bpm_);
    }
}

void Sequencer::rearm() {
    if (clock_src_ != ClockSource::EXT) return;
    play_state_ = PlayState::ARMED;
    uint32_t s  = save_and_disable_interrupts();
    ticks_pending_ = 0;
    restore_interrupts(s);
    LOG_AUDIO("SEQ: RE-ARMED");
}

void Sequencer::stop() {
    play_state_ = PlayState::STOPPED;
    armed_record_ = false;
    tick_ = 0;
    play_step_ = 0;
    last_step_ = 0xFF;
    active_fx_mask_ = 0;
    active_note_mask_ = 0;
    std::memset(active_note_midi_, 0, sizeof(active_note_midi_));
    uint32_t s  = save_and_disable_interrupts();
    ticks_pending_ = 0;
    restore_interrupts(s);
    LOG_AUDIO("SEQ: STOP");
}

void Sequencer::rec_toggle() {
    if (play_state_ == PlayState::PLAYING) {
        play_state_ = PlayState::RECORDING;
        if (has_sequence()) {
            write_step_ = (last_step_ != 0xFF) ? last_step_ : play_step_;
            if (write_step_ >= seq_length_) write_step_ = 0;
        }
        LOG_AUDIO("SEQ: overdub ON step=%u", (unsigned)write_step_);
        return;
    }
    if (play_state_ == PlayState::RECORDING) {
        play_state_ = PlayState::PLAYING;
        LOG_AUDIO("SEQ: overdub OFF");
        return;
    }
    if (play_state_ == PlayState::STOPPED) {
        if (!manual_step_write_) {
            clear_sequence();
            manual_step_write_ = true;
            generative_enabled_ = false;
            write_step_ = 0;
            seq_length_ = 0;
            LOG_AUDIO("SEQ: step write armed");
        } else {
            if (!step_is_empty(sequence_[write_step_])) {
                if ((write_step_ + 1) > seq_length_) seq_length_ = write_step_ + 1;
            }
            sequence_valid_ = seq_length_ > 0;
            manual_step_write_ = false;
            play_step_ = 0;
            last_step_ = 0xFF;
            write_step_ = 0;
            LOG_AUDIO("SEQ: step write commit len=%u", (unsigned)seq_length_);
        }
    }
}

void Sequencer::on_manual_advance() {
    if (!manual_step_write_) return;
    if ((write_step_ + 1) > seq_length_) seq_length_ = write_step_ + 1;
    if (write_step_ < (MAX_SEQ_STEPS - 1)) {
        ++write_step_;
    }
    if (seq_length_ < (write_step_ + 1)) seq_length_ = write_step_ + 1;
    LOG_AUDIO("SEQ: write step -> %u", (unsigned)write_step_);
}

void Sequencer::record_snapshot(uint8_t slot) {
    if ((play_state_ != PlayState::RECORDING) && !manual_step_write_) return;
    if (slot >= 8) return;
    current_record_step().snapshot_mask |= (1u << slot);
    if ((write_step_ + 1) > seq_length_) seq_length_ = write_step_ + 1;
}

void Sequencer::record_note(uint8_t logical_pad, uint8_t midi_note, float) {
    if ((play_state_ != PlayState::RECORDING) && !manual_step_write_) return;
    if (logical_pad >= 8) return;
    SequenceStep& step = current_record_step();
    const uint8_t bit = (uint8_t)(1u << logical_pad);
    if (play_state_ == PlayState::RECORDING && !manual_step_write_ && (step.note_mask & bit) && step.note_midi[logical_pad] == midi_note) {
        step.note_mask &= (uint8_t)~bit;
        step.note_tie_mask &= (uint8_t)~bit;
        step.note_midi[logical_pad] = 0;
    } else {
        step.note_mask |= bit;
        step.note_tie_mask &= (uint8_t)~bit;
        step.note_midi[logical_pad] = midi_note;
    }
    if ((write_step_ + 1) > seq_length_) seq_length_ = write_step_ + 1;
}

void Sequencer::record_drum(DrumId id, float) {
    if ((play_state_ != PlayState::RECORDING) && !manual_step_write_) return;
    if ((uint8_t)id > 2u) return;
    current_record_step().drum_mask |= (1u << (uint8_t)id);
    if ((write_step_ + 1) > seq_length_) seq_length_ = write_step_ + 1;
}

void Sequencer::record_param(ParamId id, float value) {
    if ((play_state_ != PlayState::RECORDING) && !manual_step_write_) return;
    const uint8_t idx = (uint8_t)id;
    if (idx > PARAM_DUCK_AMOUNT) return;
    SequenceStep& step = current_record_step();
    step.param_mask |= (1u << idx);
    step.param_values[idx] = quantize_param(value);
    if ((write_step_ + 1) > seq_length_) seq_length_ = write_step_ + 1;
}

void Sequencer::record_fx(uint8_t fx_id, bool on) {
    if ((play_state_ != PlayState::RECORDING) && !manual_step_write_) return;
    if (fx_id > 7u) return;
    SequenceStep& step = current_record_step();
    const uint8_t bit = (1u << fx_id);
    if (on) step.fx_hold_mask |= bit;
    else    step.fx_hold_mask &= (uint8_t)~bit;
    if ((write_step_ + 1) > seq_length_) seq_length_ = write_step_ + 1;
}

void Sequencer::record_arp() {
    if ((play_state_ != PlayState::RECORDING) && !manual_step_write_) return;
    current_record_step().arp_enabled = true;
    if ((write_step_ + 1) > seq_length_) seq_length_ = write_step_ + 1;
}

void Sequencer::clear_current_snapshot_layer() {
    if (!has_sequence() && !manual_step_write_) return;
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return;
    sequence_[idx].snapshot_mask = 0;
    normalize_sequence_length();
    LOG_AUDIO("SEQ: clear snapshot layer step=%u", (unsigned)idx);
}

void Sequencer::clear_current_note(uint8_t logical_pad) {
    if ((!has_sequence() && !manual_step_write_) || logical_pad >= 8) return;
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return;
    const uint8_t bit = (uint8_t)(1u << logical_pad);
    sequence_[idx].note_mask &= (uint8_t)~bit;
    sequence_[idx].note_tie_mask &= (uint8_t)~bit;
    sequence_[idx].note_midi[logical_pad] = 0;
    normalize_sequence_length();
    LOG_AUDIO("SEQ: clear note step=%u logical=%u", (unsigned)idx, (unsigned)logical_pad);
}

void Sequencer::toggle_current_note_tie(uint8_t logical_pad) {
    if ((!has_sequence() && !manual_step_write_) || logical_pad >= 8) return;
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return;
    const uint8_t bit = (uint8_t)(1u << logical_pad);
    SequenceStep& step = sequence_[idx];
    if (step.note_mask & bit) {
        step.note_mask &= (uint8_t)~bit;
        step.note_midi[logical_pad] = 0;
    }
    step.note_tie_mask ^= bit;
    if ((idx + 1u) > seq_length_) seq_length_ = idx + 1u;
    sequence_valid_ = seq_length_ > 0;
    LOG_AUDIO("SEQ: toggle note tie step=%u logical=%u -> %s", (unsigned)idx, (unsigned)logical_pad, (step.note_tie_mask & bit) ? "ON" : "OFF");
}

void Sequencer::clear_current_drum_layer() {
    if (!has_sequence() && !manual_step_write_) return;
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return;
    sequence_[idx].drum_mask = 0;
    normalize_sequence_length();
    LOG_AUDIO("SEQ: clear drum layer step=%u", (unsigned)idx);
}

void Sequencer::clear_current_param_lock(ParamId id) {
    if (!has_sequence() && !manual_step_write_) return;
    const uint8_t idx = current_edit_step_index();
    const uint8_t bit = (uint8_t)id;
    if (idx >= MAX_SEQ_STEPS || bit > PARAM_DUCK_AMOUNT) return;
    sequence_[idx].param_mask &= ~(1u << bit);
    sequence_[idx].param_values[bit] = 0;
    normalize_sequence_length();
    LOG_AUDIO("SEQ: clear param lock step=%u param=%u", (unsigned)idx, (unsigned)bit);
}

void Sequencer::clear_current_step() {
    if (!has_sequence() && !manual_step_write_) return;
    const uint8_t idx = current_edit_step_index();
    if (idx >= MAX_SEQ_STEPS) return;
    clear_step(sequence_[idx]);
    if (manual_step_write_) write_step_ = idx;
    normalize_sequence_length();
    LOG_AUDIO("SEQ: clear step=%u", (unsigned)idx);
}

void Sequencer::duplicate_previous_step_into_next() {
    if (!manual_step_write_) return;
    const uint8_t dst = write_step_;
    const uint8_t src = (dst == 0) ? 0 : (uint8_t)(dst - 1u);
    if (dst >= MAX_SEQ_STEPS) return;
    sequence_[dst] = sequence_[src];
    if ((dst + 1u) > seq_length_) seq_length_ = dst + 1u;
    sequence_valid_ = seq_length_ > 0;
    if (write_step_ < (MAX_SEQ_STEPS - 1u)) ++write_step_;
    if ((write_step_ + 1u) > seq_length_) seq_length_ = write_step_ + 1u;
    LOG_AUDIO("SEQ: duplicate step %u -> %u", (unsigned)src, (unsigned)dst);
}

void Sequencer::copy_current_page() {
    const uint8_t base = visible_page_base();
    for (uint8_t i = 0; i < 8; ++i) {
        const uint8_t step = (uint8_t)(base + i);
        page_copy_[i] = (step < MAX_SEQ_STEPS) ? sequence_[step] : SequenceStep{};
    }
    page_copy_valid_ = true;
    LOG_AUDIO("SEQ: copied page base=%u", (unsigned)base);
}

void Sequencer::paste_page_to_current_page() {
    if (!page_copy_valid_) return;
    const uint8_t base = visible_page_base();
    for (uint8_t i = 0; i < 8; ++i) {
        const uint8_t step = (uint8_t)(base + i);
        if (step < MAX_SEQ_STEPS) sequence_[step] = page_copy_[i];
    }
    if ((base + 8u) > seq_length_) seq_length_ = (uint8_t)((base + 8u) > MAX_SEQ_STEPS ? MAX_SEQ_STEPS : (base + 8u));
    normalize_sequence_length();
    LOG_AUDIO("SEQ: pasted page base=%u", (unsigned)base);
}


uint8_t Sequencer::groove_delay_ticks_for_step(uint8_t step_index, bool drum_lane, bool note_lane) const {
    if (swing_ <= 0.001f) return 0;
    const GrooveProfile& gp = groove_profile(groove_template_);
    const uint8_t idx = step_index & 0x07u;
    float lane_scale = drum_lane ? 1.0f : (note_lane ? 0.75f : 0.0f);
    float bias = gp.timing_bias[idx] * lane_scale * (swing_ / 0.65f);
    int ticks = (int)(bias * 6.0f + 0.5f);
    if (ticks < 0) ticks = 0;
    if (ticks > 6) ticks = 6;
    return (uint8_t)ticks;
}

float Sequencer::groove_velocity_scale_for_step(uint8_t step_index, bool drum_lane, bool note_lane) const {
    const GrooveProfile& gp = groove_profile(groove_template_);
    const uint8_t idx = step_index & 0x07u;
    float lane_scale = drum_lane ? 1.0f : (note_lane ? 0.6f : 0.0f);
    float v = 1.0f + gp.velocity_bias[idx] * lane_scale * (0.5f + (swing_ / 0.65f) * 0.5f);
    return clampf_local(v, 0.80f, 1.12f);
}


bool Sequencer::enqueue_event(RingBuffer<SequencerEvent, 128>& queue, const SequencerEvent& ev, bool count_step_load) {
    if (!queue.push(ev)) {
        ++event_queue_drop_count_;
        return false;
    }
    if (count_step_load) {
        if (current_step_event_count_ < 255) ++current_step_event_count_;
        if (current_step_event_count_ > max_events_per_step_) max_events_per_step_ = current_step_event_count_;
    }
    update_queue_metrics(queue);
    return true;
}

void Sequencer::update_queue_metrics(const RingBuffer<SequencerEvent, 128>& queue) {
    const uint8_t used = queue.size();
    if (used > event_queue_high_water_) event_queue_high_water_ = used;
}

void Sequencer::update_pending_metrics() {
    uint8_t used = 0;
    for (const auto &p : pending_) {
        if (p.active) ++used;
    }
    if (used > pending_high_water_) pending_high_water_ = used;
}

void Sequencer::schedule_event(const SequencerEvent& ev, uint32_t delay_ticks) {
    const uint32_t due = tick_ + delay_ticks;
    for (auto &p : pending_) {
        if (!p.active) {
            p.active = true;
            p.due_tick = due;
            p.ev = ev;
            p.ev.tick = due;
            update_pending_metrics();
            return;
        }
    }
    ++pending_overflow_count_;
}

void Sequencer::flush_pending_events(RingBuffer<SequencerEvent, 128>& queue) {
    for (auto &p : pending_) {
        if (!p.active) continue;
        if ((int32_t)(tick_ - p.due_tick) >= 0) {
            if (enqueue_event(queue, p.ev, true)) {
                p.active = false;
            }
        }
    }
    update_pending_metrics();
}

void Sequencer::emit_param_lock(ParamId id, float value, RingBuffer<SequencerEvent, 128>& queue) {
    switch (id) {
    case PARAM_DRUM_DECAY:
        enqueue_event(queue, {tick_, EVT_DRUM_PARAM, (uint8_t)DRUM_PARAM_DECAY, value}, true);
        break;
    case PARAM_DRUM_COLOR:
        enqueue_event(queue, {tick_, EVT_DRUM_PARAM, (uint8_t)DRUM_PARAM_COLOR, value}, true);
        break;
    case PARAM_DUCK_AMOUNT:
        enqueue_event(queue, {tick_, EVT_DRUM_PARAM, (uint8_t)DRUM_PARAM_DUCK, value}, true);
        break;
    default:
        enqueue_event(queue, {tick_, EVT_PARAM_CHANGE, (uint8_t)id, value}, true);
        break;
    }
}

void Sequencer::emit_step(const SequenceStep& step, RingBuffer<SequencerEvent, 128>& queue) {
    const bool step_pass = (step.chance_q8 >= 255) || ((rng_next() & 0xFFu) <= step.chance_q8);
    const uint8_t step_idx = play_step_;
    const uint8_t drum_delay = groove_delay_ticks_for_step(step_idx, true, false);
    const uint8_t note_delay = groove_delay_ticks_for_step(step_idx, false, true);
    const float drum_vel = groove_velocity_scale_for_step(step_idx, true, false);
    const float note_vel = groove_velocity_scale_for_step(step_idx, false, true);

    for (uint8_t logical = 0; logical < 8; ++logical) {
        const uint8_t bit = (uint8_t)(1u << logical);
        const bool had_note = (active_note_mask_ & bit) != 0;
        const bool has_note = step_pass && ((step.note_mask & bit) != 0);
        const bool has_tie  = step_pass && ((step.note_tie_mask & bit) != 0);
        const uint8_t prev_midi = active_note_midi_[logical];
        const uint8_t next_midi = has_note ? step.note_midi[logical] : (has_tie ? prev_midi : 0);

        if (had_note && !has_note && !has_tie) {
            enqueue_event(queue, {tick_, EVT_NOTE_OFF, prev_midi, 0.0f}, true);
            active_note_midi_[logical] = 0;
        } else if (had_note && has_note && prev_midi != next_midi) {
            enqueue_event(queue, {tick_, EVT_NOTE_OFF, prev_midi, 0.0f}, true);
            if (note_delay) schedule_event({tick_, EVT_NOTE_ON, next_midi, 0.85f * note_vel}, note_delay);
            else enqueue_event(queue, {tick_, EVT_NOTE_ON, next_midi, 0.85f * note_vel}, true);
            active_note_midi_[logical] = next_midi;
        } else if (!had_note && has_note) {
            if (note_delay) schedule_event({tick_, EVT_NOTE_ON, next_midi, 0.85f * note_vel}, note_delay);
            else enqueue_event(queue, {tick_, EVT_NOTE_ON, next_midi, 0.85f * note_vel}, true);
            active_note_midi_[logical] = next_midi;
        } else if (had_note && has_tie) {
            active_note_midi_[logical] = prev_midi;
        }
    }
    active_note_mask_ = step_pass ? (uint8_t)(step.note_mask | step.note_tie_mask) : 0;

    const uint8_t next_fx = step_pass ? step.fx_hold_mask : 0;
    const uint8_t fx_on = (uint8_t)(next_fx & ~active_fx_mask_);
    const uint8_t fx_off = (uint8_t)(active_fx_mask_ & ~next_fx);
    for (uint8_t i = 0; i < 8; ++i) {
        const uint8_t bit = (1u << i);
        if (fx_on & bit)  enqueue_event(queue, {tick_, EVT_FX_ON, i, 1.0f}, true);
        if (fx_off & bit) enqueue_event(queue, {tick_, EVT_FX_OFF, i, 0.0f}, true);
    }
    active_fx_mask_ = next_fx;

    for (uint8_t slot = 0; slot < 8; ++slot) {
        if (step_pass && (step.snapshot_mask & (1u << slot))) {
            enqueue_event(queue, {tick_, EVT_PAD_TRIGGER, slot, 1.0f}, true);
        }
    }
    if (step_pass && step.arp_enabled) {
        enqueue_event(queue, {tick_, EVT_PAD_TRIGGER, (uint8_t)(arp_play_slot_ & 0x07u), 0.92f}, true);
        arp_play_slot_ = (uint8_t)((arp_play_slot_ + 1u) & 0x07u);
    }
    if (step_pass && (step.drum_mask & (1u << DRUM_KICK)))  { if (drum_delay) schedule_event({tick_, EVT_DRUM_HIT, (uint8_t)DRUM_KICK, 1.0f * drum_vel}, drum_delay); else enqueue_event(queue, {tick_, EVT_DRUM_HIT, (uint8_t)DRUM_KICK, 1.0f * drum_vel}, true); }
    if (step_pass && (step.drum_mask & (1u << DRUM_SNARE))) { if (drum_delay) schedule_event({tick_, EVT_DRUM_HIT, (uint8_t)DRUM_SNARE, 1.0f * drum_vel}, drum_delay); else enqueue_event(queue, {tick_, EVT_DRUM_HIT, (uint8_t)DRUM_SNARE, 1.0f * drum_vel}, true); }
    if (step_pass && (step.drum_mask & (1u << DRUM_HAT)))   { if (drum_delay) schedule_event({tick_, EVT_DRUM_HIT, (uint8_t)DRUM_HAT, 0.85f * drum_vel}, drum_delay); else enqueue_event(queue, {tick_, EVT_DRUM_HIT, (uint8_t)DRUM_HAT, 0.85f * drum_vel}, true); }

    for (uint8_t idx = 0; idx <= PARAM_DUCK_AMOUNT; ++idx) {
        if (step_pass && (step.param_mask & (1u << idx))) {
            emit_param_lock((ParamId)idx, dequantize_param(step.param_values[idx]), queue);
        }
    }
}

void Sequencer::tick(RingBuffer<SequencerEvent, 128>& queue) {
    while (ticks_pending_ > 0) {
        uint32_t s = save_and_disable_interrupts();
        bool have  = ticks_pending_ > 0;
        if (have) ticks_pending_--;
        restore_interrupts(s);
        if (have) advance_tick(queue);
    }
}

void Sequencer::advance_tick(RingBuffer<SequencerEvent, 128>& queue) {
    tick_++;
    flush_pending_events(queue);

    if (clock_out_ && (tick_ % INT_MULT == 0))
        clock_out_->on_tick();

    const bool step_boundary = ((tick_ - 1u) % STEP_TICKS) == 0u;

    if (armed_record_ && step_boundary) {
        const uint8_t count_idx = (uint8_t)(16u - preroll_steps_left_);
        if ((count_idx % 4u) == 0u) {
            enqueue_event(queue, {tick_, EVT_DRUM_HIT, (uint8_t)DRUM_HAT, count_idx == 0u ? 1.0f : 0.92f}, true);
        } else {
            enqueue_event(queue, {tick_, EVT_DRUM_HIT, (uint8_t)DRUM_KICK, 0.45f}, true);
        }
        if (preroll_steps_left_ > 0) --preroll_steps_left_;
        if (preroll_steps_left_ == 0) {
            armed_record_ = false;
            manual_step_write_ = true;
            play_state_ = PlayState::RECORDING;
            clear_sequence();
            LOG_AUDIO("SEQ: preroll done -> recording");
        }
    }

    if (has_sequence() && (play_state_ == PlayState::PLAYING || play_state_ == PlayState::RECORDING) && step_boundary) {
        current_step_event_count_ = 0;
        if (play_step_ >= seq_length_) play_step_ = 0;
        emit_step(sequence_[play_step_], queue);
        last_step_ = play_step_;
        if (play_state_ == PlayState::RECORDING) {
            write_step_ = play_step_;
        }
        play_step_ = (uint8_t)((play_step_ + 1u) % (seq_length_ ? seq_length_ : 1u));
    }

    run_generative_step(queue);

    if (++debug_counter_ >= INT_PPQN) {
        debug_counter_ = 0;
        update_queue_metrics(queue);
        LOG("SEQ tick=%lu %s %.1fBPM swing=%.2f state=%d pendUsed=%u/%u qUsed=%u/%u drops=%lu pOvf=%lu len=%u step=%u evMax=%u",
            (unsigned long)tick_,
            clock_src_ == ClockSource::EXT ? "EXT" : "INT",
            bpm_, swing_, (int)play_state_,
            (unsigned)pending_high_water_, (unsigned)(sizeof(pending_) / sizeof(pending_[0])),
            (unsigned)queue.size(), (unsigned)RingBuffer<SequencerEvent, 128>::capacity(),
            (unsigned long)event_queue_drop_count_, (unsigned long)pending_overflow_count_,
            (unsigned)seq_length_, (unsigned)play_step_, (unsigned)max_events_per_step_);
    }
}


void Sequencer::panic_restore(RingBuffer<SequencerEvent, 128>& queue) {
    for (uint8_t logical = 0; logical < 8; ++logical) {
        const uint8_t bit = (uint8_t)(1u << logical);
        if ((active_note_mask_ & bit) != 0) {
            const uint8_t midi = active_note_midi_[logical];
            if (midi != 0) {
                enqueue_event(queue, {tick_, EVT_NOTE_OFF, midi, 0.0f}, true);
            }
            active_note_midi_[logical] = 0;
        }
    }
    active_note_mask_ = 0;

    for (uint8_t i = 0; i < 8; ++i) {
        const uint8_t bit = (uint8_t)(1u << i);
        if ((active_fx_mask_ & bit) != 0) {
            enqueue_event(queue, {tick_, EVT_FX_OFF, i, 0.0f}, true);
        }
    }
    active_fx_mask_ = 0;
}
