#pragma once
// state_manager.h — Bytebeat Machine V1.14
// Estado central del firmware: snapshots, parámetros live, encoder y
// ruteo coherente entre patch, bus FX y controles globales.
#include <cstdint>
#include "snapshot_event.h"
#include "encoder_state.h"
#include "../synth/bytebeat_graph.h"
#include "../synth/quantizer.h"
#include "../synth/glide.h"
#include "hardware/sync.h"
#include "../sequencer/event_types.h"

struct Snapshot {
    uint32_t seed;
    uint8_t  zone;
    float    macro;
    float    glide_time;
    float    time_div;
    float    tonal;
    float    spread;
    float    filter_cutoff;
    float    fx_amount;
    float    drive;
    float    reverb_room;   // 0.0-1.0, default 0.84
    float    reverb_wet;    // 0.0-1.0, default 0.25
    // V1.6
    uint8_t  scale_id;      // ScaleId 0-7, default MAJOR(1)
    uint8_t  root;          // 0-11, default C(0)
    float    drum_color;    // 0.0-1.0 (family selector), default 0.0
    float    drum_decay;    // 0.0-1.0 (decay scale), default 0.5
    // V1.10 — Envelope
    float    env_release;   // 0.0-1.0 → 1ms..8s, default 0.0 (snappy)
    float    env_attack;    // 0.0-1.0 → 1ms..600ms, default 0.0
    bool     env_loop;      // default false
    bool     valid;
};

static constexpr float   TIME_DIV_STEPS[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
static constexpr uint8_t TIME_DIV_COUNT   = 5;

inline float quantize_time_div(float pot_val) {
    uint8_t idx = (uint8_t)(pot_val * (TIME_DIV_COUNT - 1) + 0.5f);
    if (idx >= TIME_DIV_COUNT) idx = TIME_DIV_COUNT - 1;
    return TIME_DIV_STEPS[idx];
}

enum class RandomizeMode : uint8_t { CONTROLLED = 0, WILD = 1 };

class StateManager {
public:
    static constexpr uint8_t NUM_SNAPSHOTS = 8;
    static constexpr uint8_t NO_PENDING    = 0xFF;

    void init();

    // ── Core0 only ───────────────────────────────────────────
    void    process_pending();
    void    fill_context(EvalContext& ctx) const;  // no toca ctx.t
    int16_t evaluate(const EvalContext& ctx);

    uint8_t get_active_slot()  const { return active_slot_; }

    // ── Core1 ────────────────────────────────────────────────
    void request_trigger(uint8_t slot);
    void request_save   (uint8_t slot, const float pots[6]);
    void set_patch_param(ParamId id, float value);
    void set_bus_param  (ParamId id, float value);
    void set_aftertouch_macro(float pressure);
    void randomize_all(RandomizeMode mode);
    void mutate_active_snapshot(float amount, bool wild = false);

    // Quantizer / root / scale (Core1)
    void set_scale(uint8_t sid);
    void set_root(uint8_t r);

    // Encoder contextual (preparación V1.14)
    void next_encoder_mode();

    // HOME: resetea el estado de performance sin tocar snapshots ni secuencia.
    // Nivel 1 (soft): encoder → BPM, bus params (chorus, hp, reverb) → defaults
    //                 del snapshot activo, pot virtual tracking recalibrado.
    // Nivel 2 (full): ídem nivel 1 + drum mutes → false + punch FX activos off.
    enum class HomeLevel : uint8_t { SOFT = 0, FULL = 1 };
    void home_reset(HomeLevel level);
    void encoder_delta(int delta, bool shifted = false);
    EncoderMode get_encoder_mode() const { return encoder_.mode; }
    const EncoderState& get_encoder_state() const { return encoder_; }
    float get_mutate_amount() const { return encoder_.mutate_amount; }
    float get_swing_amount()  const { return encoder_.swing_amount;  }

    // Drum/bus live controls (Core1 escribe, Core0 lee en safe points).
    void set_drum_color_live(float v) {
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        drum_color_live_ = v;
    }
    void set_drum_decay_live(float v) {
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        drum_decay_live_ = v;
    }
    void set_duck_amount_live(float v) {
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        duck_amount_live_ = v;
    }
    // Bus FX live setters (layout final de 3 capas).
    void set_reverb_wet_live(float v) {
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        reverb_wet_live_ = v;
    }
    void set_chorus_live  (float v) { chorus_live_   = clamp01(v); }
    void set_hp_live      (float v) { hp_live_       = clamp01(v); }
    void set_grain_live   (float v) { grain_live_    = clamp01(v); }
    void set_snap_live    (float v) { snap_live_     = clamp01(v); }
    void set_stutter_rate_live(float v) { stutter_rate_live_ = clamp01(v); }
    // Patch/envelope live setters.
    void set_env_release (float v) { env_release_live_ = clamp01(v); }
    void set_env_attack  (float v) { env_attack_live_  = clamp01(v); }
    void set_env_loop    (bool  b) { env_loop_live_    = b;          }
    void set_env_loop_time(float v){ env_loop_time_live_ = clamp01(v); }

    float get_chorus_live()   const { return chorus_live_;  }
    float get_hp_live()       const { return hp_live_;      }
    float get_grain_live()    const { return grain_live_;   }
    float get_snap_live()     const { return snap_live_;    }
    float get_stutter_rate_live() const { return stutter_rate_live_; }
    float get_env_release ()  const { return env_release_live_;   }
    float get_env_attack  ()  const { return env_attack_live_;    }
    bool  get_env_loop    ()  const { return env_loop_live_;      }
    float get_env_loop_time() const { return env_loop_time_live_; }

    // Note Mode (Core1)
    void set_note_mode(bool active) { note_mode_active_ = active; }
    bool is_note_mode()       const { return note_mode_active_; }

    // Nota activa del Note Mode (Core1 escribe, Core0 lee en fill_context)
    // note_pitch_ratio = freq_nota / freq_base_A4
    void set_note_pitch(float ratio) { note_pitch_ratio_ = ratio; }
    void clear_note_pitch()          { note_pitch_ratio_ = 1.0f;  }

    // Flash persistence (Core1, con Core0 pausado)
    bool flash_save();
    bool flash_load();

    // V1.14: mute por canal — Core1 escribe, Core0 lee en drain_events
    void toggle_mute_drum(uint8_t i) {
        if (i == 0) mute_kick_  = !mute_kick_;
        else if (i == 1) mute_snare_ = !mute_snare_;
        else if (i == 2) mute_hat_   = !mute_hat_;
    }
    bool get_mute_drum(uint8_t i) const {
        if (i == 0) return mute_kick_;
        if (i == 1) return mute_snare_;
        if (i == 2) return mute_hat_;
        return false;
    }
    void set_mute_snap (uint8_t slot, bool v) {
        if (slot < NUM_SNAPSHOTS) mute_snap_[slot] = v;
    }
    bool get_mute_kick ()              const { return mute_kick_;       }
    bool get_mute_snare()              const { return mute_snare_;      }
    bool get_mute_hat  ()              const { return mute_hat_;        }
    bool get_mute_snap (uint8_t slot)  const {
        return (slot < NUM_SNAPSHOTS) ? mute_snap_[slot] : false;
    }


    // Getters (Core0 lee en safe point)
    float   get_drive_live()      const { return drive_live_;        }
    float   get_glide_live()      const { return glide_live_;        }
    float   get_reverb_room()     const { return reverb_room_live_;  }
    float   get_reverb_wet()      const { return reverb_wet_live_;   }
    uint8_t get_scale_id()        const { return scale_id_live_;     }
    uint8_t get_root()            const { return root_live_;         }
    float   get_drum_color_live()  const { return drum_color_live_;   }
    float   get_drum_decay_live()  const { return drum_decay_live_;   }
    float   get_duck_amount_live() const { return duck_amount_live_;  }

    // Acceso a snapshots para FlashStore
    const Snapshot* get_snapshots() const { return snapshots_; }
    Snapshot*       get_snapshots()       { return snapshots_; }

private:
    void     do_trigger(uint8_t slot);
    void     do_save   (uint8_t slot, const float pots[6]);
    uint32_t generate_seed(uint8_t slot);
    uint32_t rng_next();
    float    rand01();
    float    rand_range(float lo, float hi);
    uint8_t  rand_u8(uint8_t max_exclusive);
    static ParamId patch_param_from_pot(uint8_t pot_idx);
    static ParamId bus_param_from_pot(uint8_t pot_idx);

    BytebeatGraph graphs_[2];
    uint8_t       active_graph_   = 0;
    uint8_t       incoming_graph_ = 1;
    Glide         glide_;
    uint8_t       pending_slot_   = NO_PENDING;
    uint8_t       active_slot_    = 0;
    Snapshot      snapshots_[NUM_SNAPSHOTS] = {};
    EvalContext   ctx_ = {};

    // Live fields — escritos por Core1, leídos por Core0 en safe points
    float         reverb_room_live_ = 0.84f;
    float         reverb_wet_live_  = 0.25f;
    float         drive_live_       = 0.0f;
    float         glide_live_       = 0.1f;
    uint8_t       scale_id_live_    = 1;    // MAJOR
    uint8_t       root_live_        = 0;    // C
    float         drum_color_live_  = 0.0f;
    float         drum_decay_live_  = 0.5f;
    float         duck_amount_live_ = 0.0f;

    // V1.7 — Note Mode live fields
    volatile bool  note_mode_active_ = false;  // Core1 escribe, Core0 lee
    volatile float note_pitch_ratio_ = 1.0f;   // ratio de la nota activa

    // Bus FX live fields (no pertenecen al snapshot).
    float          chorus_live_  = 0.0f;
    float          hp_live_      = 0.0f;
    float          grain_live_   = 0.0f;
    float          snap_live_    = 0.0f;
    float          stutter_rate_live_ = 0.5f;
    // V1.10 — Envelope live fields (Core1 escribe, Core0 lee)
    volatile float env_release_live_   = 0.0f;
    volatile float env_attack_live_    = 0.0f;
    volatile bool  env_loop_live_      = false;
    volatile float env_loop_time_live_ = 1.0f;
    EncoderState   encoder_            = {};
    uint32_t       random_state_       = 0x13579BDFu;

    static float clamp01(float v) {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }

    SnapshotEvent pending_event_;
    bool          event_ready_  = false;
    spin_lock_t*  lock_         = nullptr;

    // Mute state — Core1 escribe via setters, Core0 lee en drain_events
    bool          mute_kick_         = false;
    bool          mute_snare_        = false;
    bool          mute_hat_          = false;
    bool          mute_snap_[NUM_SNAPSHOTS] = {};
};
