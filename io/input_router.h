#pragma once
// input_router.h — control routing and pad layout
// Layout físico 3x5 del firmware actual:
//
//         COL0      COL1      COL2      COL3      COL4
// ROW0  [ REC ][ PLAY ][ SHIFT ][  C  ][  F  ]
// ROW1  [MUTE ][ HAT  ][  A   ][  D  ][  G  ]
// ROW2  [KICK ][SNARE ][  B   ][  E  ][  H  ]
//
// Los 8 pads melódicos/snapshots viven en posiciones físicas no contiguas.
// Por eso usamos un mapeo físico -> slot lógico.

#include "cap_pad_handler.h"
#include "adc_handler.h"
#include "encoder.h"
#include "../sequencer/sequencer.h"
#include "../state/state_manager.h"
#include "../utils/ring_buffer.h"
#include "../sequencer/event_types.h"
#include "../midi/uart_midi.h"
#include "../synth/note_mode.h"

// Pads físicos fijos del layout nuevo
static constexpr uint8_t PAD_REC     =  0;
static constexpr uint8_t PAD_PLAY    =  1;
static constexpr uint8_t PAD_SHIFT   =  2;
static constexpr uint8_t PAD_MUTE =  5;
static constexpr uint8_t PAD_HAT     =  6;
static constexpr uint8_t PAD_KICK    = 10;
static constexpr uint8_t PAD_SNARE   = 11;

// Pads físicos donde viven snapshots / notas
// Orden lógico elegido:
//   slot0=C, slot1=F, slot2=A, slot3=D,
//   slot4=G, slot5=B, slot6=E, slot7=H
static constexpr uint8_t SNAP_PAD_PHYS[8] = {3, 4, 7, 8, 9, 12, 13, 14};

// Mapa físico -> slot lógico de snapshot / índice lógico de note mode
// 0xFF = no es snapshot/note pad
static constexpr uint8_t SNAP_SLOT_FROM_PHYS[CapPadHandler::NUM_PADS] = {
    0xFF, 0xFF, 0xFF, 0, 1,
    0xFF, 0xFF, 2, 3, 4,
    0xFF, 0xFF, 5, 6, 7
};

static constexpr uint8_t NOTE_PAD_COUNT = 8;
static constexpr uint8_t INVALID_SLOT   = 0xFF;

enum class AftertouchMode : uint8_t {
    // Comportamiento fijo por pad (no depende de este enum):
    //   PAD_MUTE  → grain freeze wet (presión = profundidad de congelación)
    //   SNAP pads → reverb wet momentáneo (presión = apertura espacial)
    //   Note Mode → velocity de nota (siempre activo si note_mode_)
    //
    // Este enum controla solo el comportamiento de los pads de transport
    // (KICK, SNARE, HAT, REC, PLAY) que no tienen rol aftertouch definido.
    NONE          = 0,   // pads sin rol aftertouch no emiten nada (default)
    STUTTER_DEPTH = 1,   // legacy: mantener compatibilidad con código externo
};

// Targets internos de EVT_AFTERTOUCH (ev.target):
//   PAD_MUTE (5)  → grain freeze wet
//   0xFD          → reverb wet momentáneo (SNAP pads)
//   nota MIDI     → velocity en Note Mode
// AT_TARGET_REVERB_SNAP definido en sequencer/event_types.h

class LedController;

class InputRouter {
public:
    AftertouchMode aftertouch_mode = AftertouchMode::STUTTER_DEPTH;

    UartMidi*     midi     = nullptr;
    LedController* led_ctrl = nullptr;

    void process(CapPadHandler& pads, AdcHandler& adc,
                 Sequencer& seq, StateManager& state,
                 RingBuffer<SequencerEvent, 128>& queue);

private:
    void handle_transport  (CapPadHandler& pads, Sequencer& seq,
                            StateManager& state, bool shift, uint16_t just_on,
                            RingBuffer<SequencerEvent, 128>& queue);
    void handle_snapshots  (CapPadHandler& pads, Sequencer& seq,
                            StateManager& state, RingBuffer<SequencerEvent, 128>& queue,
                            uint16_t just_on);
    void handle_note_pads  (CapPadHandler& pads, Sequencer& seq, StateManager& state,
                            RingBuffer<SequencerEvent, 128>& queue, uint16_t just_on,
                            uint16_t just_off);
    void handle_drums      (CapPadHandler& pads, Sequencer& seq,
                            StateManager& state,
                            RingBuffer<SequencerEvent, 128>& queue);
    void handle_encoder    (CapPadHandler& pads, Sequencer& seq, StateManager& state);
    void handle_adc        (CapPadHandler& pads, AdcHandler& adc, Sequencer& seq, StateManager& state,
                            RingBuffer<SequencerEvent, 128>& queue);
    ParamId resolve_pot_param(uint8_t pot_index, bool shift, bool shift_rec) const;
    bool is_drum_bus_param(ParamId id) const;
    void handle_aftertouch (CapPadHandler& pads, StateManager& state,
                            RingBuffer<SequencerEvent, 128>& queue, Sequencer& seq);

    float    last_pot_      [6]                       = {};
    uint32_t drum_press_ms_ [3]                       = {};
    bool     drum_rolling_  [3]                       = {};
    float    last_pressure_ [CapPadHandler::NUM_PADS] = {};
    bool     last_shift_                               = false;
    uint32_t snap_press_ms_ [8] = {};
    bool     snap_saved_    [8] = {};
    static constexpr uint32_t SNAP_HOLD_ACTION_MS = 650u;
    bool     note_mode_        = false;
    uint8_t  active_note_midi_ [NOTE_PAD_COUNT] = {};
    bool     note_active_      [NOTE_PAD_COUNT] = {}; 
    bool     shift_rec_mode_   = false;
    bool     random_combo_active_ = false;
    Encoder  encoder_;
    bool     encoder_ready_ = false;
    bool     random_wild_fired_   = false;
    uint32_t random_combo_ms_     = 0;
    bool     snap_arp_active_  = false;
    uint8_t  snap_arp_step_    = 0;
    uint32_t snap_arp_next_ms_ = 0;
    bool     shift_used_for_action_ = false;
    uint32_t last_shift_tap_ms_ = 0;
    bool     shift_waiting_second_tap_ = false;

    // ── Soft takeover ────────────────────────────────────────────
    // Cuando se cambia de capa, el pot queda en estado CATCHING hasta que
    // la posición física cruce el valor virtual guardado. Al cruzar → TRACKING.
    enum class PotState : uint8_t { TRACKING = 0, CATCHING };
    PotState pot_state_     [6] = {};
    float    pot_catch_val_ [6] = {};  // valor virtual al momento del cambio de capa
    // Capa anterior para detectar cambios
    uint8_t  last_layer_ = 0;  // 0=normal, 1=shift, 2=shift+rec

    // ── HOME via hold del encoder ────────────────────────────────
    // El hold del encoder en dos niveles:
    //   700ms – 1500ms → SOFT (encoder+bus params)
    //   > 1500ms       → FULL (+ mutes + drum params)
    bool     home_full_fired_ = false;
    bool     encoder_hold_started_  = false;
    uint32_t encoder_hold_start_ms_ = 0;
    static constexpr uint32_t HOME_FULL_MS = 1500u;

    // Punch FX activos: bitmask de los 7 FX (bits 0–6 = slots 0–6)
    uint8_t  active_punch_fx_ = 0;

    // Randomize: protección contra disparo accidental
    // Solo se dispara si la combinación se mantiene RANDOM_GUARD_MS
    static constexpr uint32_t RANDOM_GUARD_MS = 400u;
};
