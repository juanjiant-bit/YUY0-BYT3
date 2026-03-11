#include "../synth/quantizer.h"
// state_manager.cpp — Bytebeat Machine V1.10
// V1.10: env fields in Snapshot, set_param cases 5, do_save pots[6]
// Cambios V1.7:
//   + fill_context() exporta drum_color/decay, note_mode_active, note_pitch_ratio
//   + do_trigger() interpola drum params (glide entre snapshots)
#include "state_manager.h"
#include "flash_store.h"
#include "../audio/audio_engine.h"
#include "../utils/debug_log.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/sync.h"

namespace {
inline float clampf01_local(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

inline float jitter_value(float current, float amount, float span, float r01) {
    const float delta = (r01 * 2.0f - 1.0f) * span * amount;
    return clampf01_local(current + delta);
}

inline float biased_jitter_value(float current, float amount, float span, float r01) {
    const float signed_bias = (r01 * 2.0f - 1.0f);
    const float shaped = signed_bias * signed_bias * signed_bias;
    return clampf01_local(current + shaped * span * amount);
}

inline float jitter_in_range(float current, float amount, float span, float lo, float hi, float r01) {
    const float signed_bias = (r01 * 2.0f - 1.0f);
    const float shaped = signed_bias * signed_bias * signed_bias;
    float v = current + shaped * span * amount;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

inline uint8_t quantized_time_div_index(float td) {
    uint8_t best = 0;
    float best_err = 9999.0f;
    for (uint8_t i = 0; i < TIME_DIV_COUNT; ++i) {
        float err = TIME_DIV_STEPS[i] - td;
        if (err < 0.0f) err = -err;
        if (err < best_err) {
            best_err = err;
            best = i;
        }
    }
    return best;
}
}

void StateManager::init() {
    uint lock_num = spin_lock_claim_unused(true);
    lock_         = spin_lock_instance(lock_num);

    for (uint8_t i = 0; i < NUM_SNAPSHOTS; i++) {
        snapshots_[i] = { generate_seed(i), 2, 0.5f, 0.1f, 1.0f,
                          0.0f, 0.0f, 1.0f, 0.2f, 0.0f, 0.84f, 0.25f,
                          1,    // scale_id: MAJOR
                          0,    // root: C
                          0.0f, 0.5f,  // drum_color, drum_decay
                          0.0f, 0.0f, false,  // env_release, env_attack, env_loop
                          true };
    }

    if (FlashStore::load(snapshots_)) {
        LOG_AUDIO("STATE: snapshots cargados desde Flash OK");
    } else {
        LOG_AUDIO("STATE: Flash inválido — usando defaults de fábrica");
    }

    ZoneConfig cfg = make_zone(snapshots_[0].zone);
    graphs_[active_graph_].generate(snapshots_[0].seed, snapshots_[0].zone, cfg);

    active_slot_   = 0;
    ctx_.zone      = snapshots_[0].zone;
    ctx_.macro     = snapshots_[0].macro;
    ctx_.tonal     = snapshots_[0].tonal;
    ctx_.time_div  = snapshots_[0].time_div;
    ctx_.spread    = snapshots_[0].spread;
    ctx_.t         = 0;
    ctx_.note_mode_active = false;
    ctx_.note_pitch_ratio = 1.0f;
    ctx_.drum_color = snapshots_[0].drum_color;
    ctx_.drum_decay = snapshots_[0].drum_decay;

    scale_id_live_   = snapshots_[0].scale_id;
    root_live_       = snapshots_[0].root;
    drum_color_live_ = snapshots_[0].drum_color;
    drum_decay_live_ = snapshots_[0].drum_decay;
    drive_live_      = snapshots_[0].drive;
    glide_live_      = snapshots_[0].glide_time;
    reverb_room_live_= snapshots_[0].reverb_room;
    reverb_wet_live_ = snapshots_[0].reverb_wet;
    duck_amount_live_= 0.0f;
    encoder_.root     = root_live_;
    encoder_.scale_id = scale_id_live_;

    env_release_live_   = snapshots_[0].env_release;
    env_attack_live_    = snapshots_[0].env_attack;
    env_loop_live_      = snapshots_[0].env_loop;
    env_loop_time_live_ = 1.0f;

    LOG_AUDIO("STATE: init OK — slot0 seed=0x%08lX zone=%u",
              (unsigned long)snapshots_[0].seed, snapshots_[0].zone);
}

uint32_t StateManager::generate_seed(uint8_t slot) {
    return 0xDEADBEEFu ^ (0x9E3779B9u * (uint32_t)(slot + 1));
}

uint32_t StateManager::rng_next() {
    random_state_ ^= (random_state_ << 13);
    random_state_ ^= (random_state_ >> 17);
    random_state_ ^= (random_state_ << 5);
    return random_state_;
}

float StateManager::rand01() {
    return (float)(rng_next() & 0x00FFFFFFu) / 16777215.0f;
}

float StateManager::rand_range(float lo, float hi) {
    return lo + (hi - lo) * rand01();
}

uint8_t StateManager::rand_u8(uint8_t max_exclusive) {
    return max_exclusive ? (uint8_t)(rng_next() % max_exclusive) : 0;
}


void StateManager::process_pending() {
    if (!glide_.is_active() && pending_slot_ != NO_PENDING) {
        active_graph_ = incoming_graph_;
        active_slot_  = pending_slot_;
        pending_slot_ = NO_PENDING;
        LOG_AUDIO("STATE: commit slot %u", active_slot_);
    }

    SnapshotEvent ev;
    bool has_ev = false;
    {
        uint32_t s = spin_lock_blocking(lock_);
        if (event_ready_) {
            ev          = pending_event_;
            event_ready_= false;
            has_ev      = true;
        }
        spin_unlock(lock_, s);
    }
    if (!has_ev) return;

    switch (ev.type) {
    case SnapshotEventType::TRIGGER: do_trigger(ev.slot);        break;
    case SnapshotEventType::SAVE:    do_save(ev.slot, ev.pots);  break;
    default: break;
    }
}

void StateManager::do_trigger(uint8_t slot) {
    if (slot >= NUM_SNAPSHOTS || !snapshots_[slot].valid) return;
    if (slot == active_slot_ && !glide_.is_active())      return;

    const Snapshot& s = snapshots_[slot];;
    incoming_graph_   = 1 - active_graph_;

    ZoneConfig cfg = make_zone(s.zone);
    graphs_[incoming_graph_].generate(s.seed, s.zone, cfg);

    uint32_t dur = (uint32_t)(s.glide_time * AudioEngine::SAMPLE_RATE);
    glide_.start(&graphs_[active_graph_], &graphs_[incoming_graph_], dur);
    pending_slot_ = slot;

    ctx_.zone     = s.zone;
    ctx_.macro    = s.macro;
    ctx_.tonal    = s.tonal;
    ctx_.time_div = s.time_div;
    ctx_.spread   = s.spread;

    reverb_room_live_ = s.reverb_room;
    reverb_wet_live_  = s.reverb_wet;
    drive_live_       = s.drive;
    glide_live_       = s.glide_time;
    scale_id_live_    = s.scale_id;
    root_live_        = s.root;
    encoder_.scale_id = s.scale_id;
    encoder_.root     = s.root;

    // V1.10: restaurar env del snapshot al cambiar de slot
    env_release_live_ = s.env_release;
    env_attack_live_  = s.env_attack;
    env_loop_live_    = s.env_loop;

    // V1.7: drum params interpolan también con snapshot trigger
    // (el AudioEngine los aplica al DrumEngine en el siguiente safe point)
    drum_color_live_  = s.drum_color;
    drum_decay_live_  = s.drum_decay;

    LOG_AUDIO("STATE: trigger slot=%u zona=%u glide=%.2fs", slot, s.zone, s.glide_time);
}

void StateManager::do_save(uint8_t slot, const float pots[6]) {
    Snapshot& s  = snapshots_[slot];
    s.macro      = pots[0];
    s.tonal      = pots[1];
    s.spread     = pots[2];
    s.drive      = pots[3];
    s.time_div   = quantize_time_div(pots[4]);
    s.glide_time = glide_live_;
    s.reverb_room = reverb_room_live_;
    s.reverb_wet  = reverb_wet_live_;
    s.scale_id    = scale_id_live_;
    s.root        = root_live_;
    s.drum_color  = drum_color_live_;   // guarda el valor performático actual
    s.drum_decay  = drum_decay_live_;
    s.valid       = true;
    s.seed = generate_seed(slot) ^
             (uint32_t)(to_ms_since_boot(get_absolute_time()) * 0x1Fu);
    s.env_release = env_release_live_;
    s.env_attack  = env_attack_live_;
    s.env_loop    = env_loop_live_;
    LOG_AUDIO("STATE: save slot=%u drum_color=%.2f drum_decay=%.2f",
              slot, s.drum_color, s.drum_decay);
}

void StateManager::fill_context(EvalContext& ctx) const {
    ctx.macro         = ctx_.macro;
    ctx.tonal         = ctx_.tonal;
    ctx.zone          = ctx_.zone;
    ctx.time_div      = ctx_.time_div;
    ctx.spread        = ctx_.spread;
    ctx.quant_amount  = ctx_.quant_amount;
    ctx.scale_id      = scale_id_live_;
    ctx.root          = root_live_;
    // V1.7: drum params en contexto
    ctx.drum_color    = drum_color_live_;
    ctx.drum_decay    = drum_decay_live_;
    // V1.7: Note Mode
    ctx.note_mode_active = note_mode_active_;
    ctx.note_pitch_ratio = note_pitch_ratio_;
}

int16_t StateManager::evaluate(const EvalContext& ctx) {
    if (glide_.is_active()) return glide_.evaluate(ctx);
    return graphs_[active_graph_].evaluate(ctx);
}

void StateManager::request_trigger(uint8_t slot) {
    uint32_t s = spin_lock_blocking(lock_);
    pending_event_.type = SnapshotEventType::TRIGGER;
    pending_event_.slot = slot;
    event_ready_        = true;
    spin_unlock(lock_, s);
}

void StateManager::request_save(uint8_t slot, const float pots[6]) {
    uint32_t s = spin_lock_blocking(lock_);
    pending_event_.type = SnapshotEventType::SAVE;
    pending_event_.slot = slot;
    for (uint8_t i = 0; i < 6; i++) pending_event_.pots[i] = pots[i];
    event_ready_ = true;
    spin_unlock(lock_, s);
}

ParamId StateManager::patch_param_from_pot(uint8_t pot_idx) {
    static constexpr ParamId kPatchMap[6] = {
        PARAM_MACRO,
        PARAM_TONAL,
        PARAM_SPREAD,
        PARAM_DRIVE,
        PARAM_TIME_DIV,
        PARAM_SNAP_GATE,
    };
    return kPatchMap[(pot_idx < 6) ? pot_idx : 0];
}

ParamId StateManager::bus_param_from_pot(uint8_t pot_idx) {
    static constexpr ParamId kBusMap[6] = {
        PARAM_REVERB_ROOM,
        PARAM_REVERB_WET,
        PARAM_CHORUS,
        PARAM_DRUM_DECAY,
        PARAM_DRUM_COLOR,
        PARAM_DUCK_AMOUNT,
    };
    return kBusMap[(pot_idx < 6) ? pot_idx : 0];
}

void StateManager::set_patch_param(ParamId id, float value) {
    const float v = clamp01(value);
    switch (id) {
    case PARAM_MACRO:
        ctx_.macro = v;
        break;
    case PARAM_TONAL:
        ctx_.tonal = v;
        ctx_.quant_amount = Quantizer::pot_to_amount(v);
        break;
    case PARAM_SPREAD:
        ctx_.spread = v;
        break;
    case PARAM_DRIVE:
        drive_live_ = v;
        break;
    case PARAM_TIME_DIV:
        ctx_.time_div = quantize_time_div(v);
        break;
    case PARAM_SNAP_GATE:
        snap_live_ = v;
        break;
    case PARAM_GLIDE:
        glide_live_ = v * 2.0f;
        break;
    case PARAM_ENV_ATTACK:
        env_attack_live_ = v;
        break;
    case PARAM_ENV_RELEASE:
        env_release_live_ = v;
        break;
    case PARAM_STUTTER_RATE:
        stutter_rate_live_ = v;
        break;
    case PARAM_GRAIN:
        grain_live_ = v;
        break;
    case PARAM_HP:
        hp_live_ = v;
        break;
    default:
        break;
    }
}

void StateManager::set_bus_param(ParamId id, float value) {
    const float v = clamp01(value);
    switch (id) {
    case PARAM_REVERB_ROOM:
        reverb_room_live_ = v;
        break;
    case PARAM_REVERB_WET:
        reverb_wet_live_ = v;
        break;
    case PARAM_CHORUS:
        chorus_live_ = v;
        break;
    case PARAM_DRUM_DECAY:
        drum_decay_live_ = v;
        break;
    case PARAM_DRUM_COLOR:
        drum_color_live_ = v;
        break;
    case PARAM_DUCK_AMOUNT:
        duck_amount_live_ = v;
        break;
    default:
        break;
    }
}


bool StateManager::flash_save() { return FlashStore::save(snapshots_); }
bool StateManager::flash_load() { return FlashStore::load(snapshots_);  }

void StateManager::set_aftertouch_macro(float pressure) {
    float base_macro = snapshots_[active_slot_].macro;
    if (pressure < 0.01f) {
        ctx_.macro = base_macro;
    } else {
        ctx_.macro = base_macro + pressure * (1.0f - base_macro);
    }
}
void StateManager::mutate_active_snapshot(float amount, bool wild) {
    amount = clamp01(amount);
    Snapshot& s = snapshots_[active_slot_];

    if (!s.valid) {
        s.seed          = generate_seed(active_slot_);
        s.zone          = ctx_.zone;
        s.macro         = ctx_.macro;
        s.glide_time    = glide_live_;
        s.time_div      = ctx_.time_div;
        s.tonal         = ctx_.tonal;
        s.spread        = ctx_.spread;
        s.filter_cutoff = hp_live_;
        s.fx_amount     = chorus_live_;
        s.drive         = drive_live_;
        s.reverb_room   = reverb_room_live_;
        s.reverb_wet    = reverb_wet_live_;
        s.scale_id      = scale_id_live_;
        s.root          = root_live_;
        s.drum_color    = drum_color_live_;
        s.drum_decay    = drum_decay_live_;
        s.env_release   = env_release_live_;
        s.env_attack    = env_attack_live_;
        s.env_loop      = env_loop_live_;
        s.valid         = true;
    }

    random_state_ ^= (uint32_t)to_ms_since_boot(get_absolute_time()) * 0x45D9F3Bu;
    random_state_ ^= ((uint32_t)active_slot_ + 1u) * 0x9E3779B9u;

    const float contour = wild ? (0.40f + amount * 0.35f)
                               : (0.10f + amount * 0.12f);
    const float structure_p = wild ? (0.18f + amount * 0.38f)
                                   : (0.03f + amount * 0.08f);

    s.macro       = biased_jitter_value(s.macro, contour, wild ? 0.34f : 0.16f, rand01());
    s.tonal       = jitter_in_range(s.tonal, contour, wild ? 0.36f : 0.12f, 0.18f, 0.95f, rand01());
    s.spread      = jitter_in_range(s.spread, contour, wild ? 0.30f : 0.12f, 0.00f, 0.70f, rand01());
    s.drive       = jitter_in_range(s.drive, contour, wild ? 0.32f : 0.14f, 0.00f, 0.72f, rand01());
    s.glide_time  = jitter_in_range(s.glide_time, contour, wild ? 0.42f : 0.18f, 0.02f, 1.10f, rand01());
    s.env_attack  = jitter_in_range(s.env_attack, contour, wild ? 0.22f : 0.08f, 0.0f, 0.35f, rand01());
    s.env_release = jitter_in_range(s.env_release, contour, wild ? 0.28f : 0.10f, 0.01f, 0.55f, rand01());
    s.filter_cutoff = jitter_in_range(s.filter_cutoff, contour, wild ? 0.26f : 0.10f, 0.0f, 0.55f, rand01());
    s.fx_amount     = jitter_in_range(s.fx_amount, contour, wild ? 0.28f : 0.10f, 0.0f, 0.60f, rand01());

    if (wild) {
        s.reverb_room = jitter_in_range(s.reverb_room, amount, 0.16f, 0.45f, 0.95f, rand01());
        s.reverb_wet  = jitter_in_range(s.reverb_wet, amount, 0.14f, 0.05f, 0.45f, rand01());
        s.drum_color  = jitter_in_range(s.drum_color, amount, 0.22f, 0.0f, 0.85f, rand01());
        s.drum_decay  = jitter_in_range(s.drum_decay, amount, 0.20f, 0.18f, 0.88f, rand01());
    }

    const float time_div_p = wild ? (0.10f + amount * 0.24f)
                                  : (0.01f + amount * 0.05f);
    if (rand01() < time_div_p) {
        int idx = (int)quantized_time_div_index(s.time_div);
        const int max_span = wild ? ((amount > 0.70f) ? 2 : 1) : 1;
        int step = 1 + (int)(rand01() * (float)max_span);
        if (rand01() < 0.5f) step = -step;
        idx += step;
        if (idx < 0) idx = 0;
        if (idx >= (int)TIME_DIV_COUNT) idx = TIME_DIV_COUNT - 1;
        s.time_div = TIME_DIV_STEPS[idx];
    }

    const float root_p = wild ? (0.14f + amount * 0.30f)
                              : (0.01f + amount * 0.05f);
    if (rand01() < root_p) {
        int max_step = wild ? ((amount > 0.60f) ? 5 : 2) : 1;
        int step = 1 + (int)(rand01() * (float)max_step);
        if (rand01() < 0.5f) step = -step;
        int root = (int)s.root + step;
        while (root < 0) root += 12;
        while (root > 11) root -= 12;
        s.root = (uint8_t)root;
    }

    const float scale_p = wild ? (0.10f + amount * 0.26f)
                               : (0.00f + amount * 0.03f);
    if (rand01() < scale_p) {
        static constexpr uint8_t controlled_scales[] = {
            (uint8_t)ScaleId::MAJOR,
            (uint8_t)ScaleId::NAT_MINOR,
            (uint8_t)ScaleId::DORIAN,
            (uint8_t)ScaleId::PENTA_MIN,
        };
        if (wild) {
            s.scale_id = rand_u8((uint8_t)ScaleId::NUM_SCALES);
        } else {
            s.scale_id = controlled_scales[rand_u8((uint8_t)(sizeof(controlled_scales) / sizeof(controlled_scales[0])))];
        }
    }

    if (rand01() < (wild ? (0.08f + amount * 0.22f) : (0.0f + amount * 0.02f))) {
        const uint8_t zone_count = wild ? 6u : 3u;
        const int base_zone = wild ? 0 : 1;
        s.zone = (uint8_t)(base_zone + rand_u8(zone_count));
    }

    if (rand01() < (wild ? (0.25f + amount * 0.45f) : (0.05f + amount * 0.12f))) {
        uint32_t mask = wild ? rng_next() : (rng_next() & 0x0000FFFFu);
        s.seed ^= mask;
        if (s.seed == 0) s.seed = generate_seed(active_slot_) ^ rng_next();
    }

    if (wild && rand01() < (0.08f + amount * 0.18f)) {
        s.env_loop = !s.env_loop;
    }

    ctx_.zone         = s.zone;
    ctx_.macro        = s.macro;
    ctx_.tonal        = s.tonal;
    ctx_.time_div     = s.time_div;
    ctx_.spread       = s.spread;
    ctx_.quant_amount = Quantizer::pot_to_amount(s.tonal);

    reverb_room_live_ = s.reverb_room;
    reverb_wet_live_  = s.reverb_wet;
    drive_live_       = s.drive;
    glide_live_       = s.glide_time;
    scale_id_live_    = s.scale_id;
    root_live_        = s.root;
    encoder_.scale_id = s.scale_id;
    encoder_.root     = s.root;
    drum_color_live_  = s.drum_color;
    drum_decay_live_  = s.drum_decay;
    env_release_live_ = s.env_release;
    env_attack_live_  = s.env_attack;
    env_loop_live_    = s.env_loop;

    hp_live_     = s.filter_cutoff;
    chorus_live_ = s.fx_amount;
    grain_live_  = jitter_in_range(grain_live_, amount, wild ? 0.20f : 0.06f, 0.0f, 0.55f, rand01());
    snap_live_   = jitter_in_range(snap_live_, amount, wild ? 0.18f : 0.05f, 0.0f, 0.45f, rand01());

    graphs_[active_graph_].generate(s.seed, s.zone, make_zone(s.zone));
    if (pending_slot_ != NO_PENDING) {
        graphs_[incoming_graph_].generate(s.seed ^ 0xA5A5A5A5u, s.zone, make_zone(s.zone));
    }

    LOG_AUDIO("STATE: mutate slot=%u amount=%.2f mode=%s seed=0x%08lX zone=%u root=%u scale=%u td=%.2f",
              active_slot_, amount, wild ? "WILD" : "CTRL",
              (unsigned long)s.seed, s.zone, s.root, s.scale_id, s.time_div);
}

void StateManager::randomize_all(RandomizeMode mode) {
    const bool wild = (mode == RandomizeMode::WILD);

    random_state_ ^= (uint32_t)to_ms_since_boot(get_absolute_time());
    random_state_ ^= ((uint32_t)active_slot_ + 1u) * 0x9E3779B9u;

    static constexpr uint8_t scale_choices[] = {
        (uint8_t)ScaleId::MAJOR,
        (uint8_t)ScaleId::PENTA_MIN,
        (uint8_t)ScaleId::DORIAN,
    };

    for (uint8_t i = 0; i < NUM_SNAPSHOTS; ++i) {
        Snapshot& s = snapshots_[i];
        s.seed       = generate_seed(i) ^ rng_next();
        s.zone       = wild ? rand_u8(6) : (uint8_t)(2 + rand_u8(3));
        s.macro      = wild ? rand01() : rand_range(0.15f, 0.85f);
        s.glide_time = wild ? rand_range(0.0f, 2.0f) : rand_range(0.05f, 0.90f);
        s.time_div   = TIME_DIV_STEPS[ wild ? rand_u8(TIME_DIV_COUNT)
                                            : (uint8_t)(1 + rand_u8(TIME_DIV_COUNT - 2)) ];
        s.tonal      = wild ? rand01() : rand_range(0.25f, 0.90f);
        s.spread     = wild ? rand01() : rand_range(0.05f, 0.55f);
        s.filter_cutoff = rand01();
        s.fx_amount     = wild ? rand01() : rand_range(0.1f, 0.7f);
        s.drive      = wild ? rand_range(0.0f, 1.0f) : rand_range(0.05f, 0.55f);
        s.reverb_room = wild ? rand01() : rand_range(0.45f, 0.92f);
        s.reverb_wet  = wild ? rand_range(0.0f, 0.9f) : rand_range(0.08f, 0.42f);
        s.scale_id    = scale_choices[rand_u8((uint8_t)(sizeof(scale_choices) / sizeof(scale_choices[0])))];
        s.root        = wild ? rand_u8(12) : Quantizer::FAVORITE_ROOTS[rand_u8(5)];
        s.drum_color  = wild ? rand01() : rand_range(0.10f, 0.75f);
        s.drum_decay  = wild ? rand01() : rand_range(0.25f, 0.80f);
        s.env_release = wild ? rand01() : rand_range(0.02f, 0.45f);
        s.env_attack  = wild ? rand_range(0.0f, 0.7f) : rand_range(0.0f, 0.25f);
        s.env_loop    = wild ? (rand01() > 0.55f) : (rand01() > 0.82f);
        s.valid       = true;
    }

    const Snapshot& a = snapshots_[active_slot_];
    ctx_.zone     = a.zone;
    ctx_.macro    = a.macro;
    ctx_.tonal    = a.tonal;
    ctx_.time_div = a.time_div;
    ctx_.spread   = a.spread;

    reverb_room_live_   = a.reverb_room;
    reverb_wet_live_    = a.reverb_wet;
    drive_live_         = a.drive;
    glide_live_         = a.glide_time;
    scale_id_live_      = a.scale_id;
    root_live_          = a.root;
    encoder_.scale_id   = a.scale_id;
    encoder_.root       = a.root;
    drum_color_live_    = a.drum_color;
    drum_decay_live_    = a.drum_decay;
    env_release_live_   = a.env_release;
    env_attack_live_    = a.env_attack;
    env_loop_live_      = a.env_loop;
    env_loop_time_live_ = wild ? rand_range(0.2f, 1.0f) : rand_range(0.55f, 1.0f);
    chorus_live_        = wild ? rand01() : rand_range(0.0f, 0.45f);
    hp_live_            = wild ? rand01() : rand_range(0.0f, 0.35f);
    grain_live_         = wild ? rand01() : rand_range(0.0f, 0.40f);
    snap_live_          = wild ? rand01() : rand_range(0.0f, 0.35f);

    graphs_[active_graph_].generate(a.seed, a.zone, make_zone(a.zone));
    if (pending_slot_ != NO_PENDING) {
        graphs_[incoming_graph_].generate(a.seed ^ 0xA5A5A5A5u, a.zone, make_zone(a.zone));
    }

    LOG_AUDIO("STATE: randomize_all mode=%s slot=%u seed=0x%08lX",
              wild ? "WILD" : "CTRL",
              active_slot_,
              (unsigned long)a.seed);
}


// ── Quantizer control ─────────────────────────────────────────
void StateManager::set_scale(uint8_t sid) {
    if (sid < (uint8_t)ScaleId::NUM_SCALES) {
        scale_id_live_    = sid;
        encoder_.scale_id = sid;
    }
}

void StateManager::set_root(uint8_t r) {
    if (r < 12) {
        root_live_     = r;
        encoder_.root  = r;
    }
}

void StateManager::next_encoder_mode() {
    uint8_t m = static_cast<uint8_t>(encoder_.mode);
    m = static_cast<uint8_t>((m + 1u) % static_cast<uint8_t>(EncoderMode::COUNT));
    encoder_.mode = static_cast<EncoderMode>(m);
}

void StateManager::encoder_delta(int delta, bool shifted) {
    if (delta == 0) return;

    switch (encoder_.mode) {
    case EncoderMode::BPM: {
        const int step = shifted ? 1 : (delta > 0 ? 1 : -1);
        int bpm = static_cast<int>(encoder_.bpm) + step;
        if (bpm < 20) bpm = 20;
        if (bpm > 240) bpm = 240;
        encoder_.bpm = static_cast<uint16_t>(bpm);
        break;
    }

    case EncoderMode::SWING: {
        const float step = shifted ? 0.005f : 0.02f;
        float s = encoder_.swing_amount + (delta > 0 ? step : -step);
        if (s < 0.0f) s = 0.0f;
        if (s > 1.0f) s = 1.0f;
        encoder_.swing_amount = s;
        break;
    }

    case EncoderMode::ROOT: {
        const int step = shifted ? 7 : 1;
        int r = static_cast<int>(root_live_) + (delta > 0 ? step : -step);
        while (r < 0)  r += 12;
        while (r > 11) r -= 12;
        set_root(static_cast<uint8_t>(r));
        break;
    }

    case EncoderMode::SCALE: {
        const int step = shifted ? 2 : 1;
        int s = static_cast<int>(scale_id_live_) + (delta > 0 ? step : -step);
        if (s < 0) s = 0;
        if (s >= static_cast<int>(ScaleId::NUM_SCALES)) s = static_cast<int>(ScaleId::NUM_SCALES) - 1;
        set_scale(static_cast<uint8_t>(s));
        break;
    }

    case EncoderMode::MUTATE: {
        const float step = shifted ? 0.01f : 0.05f;
        float m = encoder_.mutate_amount + (delta > 0 ? step : -step);
        if (m < 0.0f) m = 0.0f;
        if (m > 1.0f) m = 1.0f;
        encoder_.mutate_amount = m;
        break;
    }

    default:
        break;
    }
}

void StateManager::home_reset(HomeLevel level)
{
    // ── Nivel 1 (SOFT): siempre se aplica ──────────────────────
    // Encoder de vuelta a BPM (modo más seguro en live)
    encoder_.mode = EncoderMode::BPM;

    // Bus params → valores del snapshot activo actual
    const Snapshot& s = snapshots_[active_slot_];
    if (s.valid) {
        reverb_room_live_ = s.reverb_room;
        reverb_wet_live_  = s.reverb_wet;
        drive_live_       = s.drive;
        chorus_live_      = 0.0f;   // chorus no se guarda en snapshot → a cero
        hp_live_          = 0.0f;   // hp tampoco → a cero
        glide_live_       = s.glide_time;
    }

    if (level == HomeLevel::FULL) {
        // ── Nivel 2 (FULL): además resetea mutes y drum params ─
        mute_kick_  = false;
        mute_snare_ = false;
        mute_hat_   = false;
        // drum params → del snapshot activo
        if (s.valid) {
            drum_color_live_ = s.drum_color;
            drum_decay_live_ = s.drum_decay;
        }
    }

    LOG_AUDIO("STATE: home_reset level=%u slot=%u", (uint8_t)level, active_slot_);
}
