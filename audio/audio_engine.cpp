#include "../synth/quantizer.h"
// audio_engine.cpp — Bytebeat Machine V1.14
//
// Cambios vigentes:
//   + control-rate cacheado por bloque
//   + envelope evaluado una sola vez por sample
//   + swing real en secuenciador
//   + mutate snapshot + secuencia
//   + spread estéreo optimizado por segmentos para evitar doble evaluación
//     completa del graph en cada sample
#include "audio_engine.h"
#include "../state/state_manager.h"
#include "../synth/bytebeat_node.h"
#include "../dsp/dsp_chain.h"
#include "pico/stdlib.h"
#include <cmath>

static AudioEngine* g_engine = nullptr;

void AudioEngine::init(AudioOutput* output, StateManager* state) {
    output_    = output;
    state_mgr_ = state;
    output_->init();
    dsp_.init();
    drums_.init();
    lead_osc_.init();
    envelope_.init();
    if (state_mgr_) state_mgr_->fill_context(cached_ctx_);
    g_engine = this;
}

void AudioEngine::set_event_queue(RingBuffer<SequencerEvent, 128>* q) {
    event_queue_ = q;
}

bool AudioEngine::timer_callback(repeating_timer_t*) {
    if (g_engine) g_engine->generate_samples();
    return true;
}

void AudioEngine::run() {
    add_repeating_timer_ms(-1, timer_callback, nullptr, &timer_);
    while (true) tight_loop_contents();
}

void AudioEngine::generate_samples() {
    accumulator_ += ACCUM_ADD;
    while (accumulator_ >= ACCUM_TOP) {
        accumulator_ -= ACCUM_TOP;
        process_one_sample();
        sample_tick_++;
    }
}

void AudioEngine::update_macro_motion(int32_t bb_i, float macro) {
    // ── Macro pot smoothing ───────────────────────────────────
    // Coeficiente lento (0.01) para evitar zipper noise cuando macro viene
    // de ADC/CV. fc ≈ 70 Hz — invisible como artefacto, rápido al oído.
    const float macro_in = clamp01(macro);
    macro_motion_.macro_s += (macro_in - macro_motion_.macro_s) * 0.01f;
    const float m = macro_motion_.macro_s;

    const uint32_t u = uint32_t(bb_i);

    // ── RHYTHM: tasa de cambio sample-a-sample ────────────────
    // Detecta el ritmo real del bytebeat midiendo cuánto cambia en cada
    // muestra. Rápido con bytebeats densos, lento con bytebeats tonales.
    // bits[11:4] captura el contenido de frecuencia media del sample.
    const float rhythm_bits = float((u >> 4) & 0xFFu) * (1.0f / 255.0f);
    const float transient   = clamp01(fabsf(float(bb_i - macro_motion_.prev_bb)) * (1.0f / 32768.0f));
    macro_motion_.rhythm_raw = 0.6f * rhythm_bits + 0.4f * transient;

    // ── CHAOS: ruido real via LFSR Galois de 16 bits ──────────
    // Un XOR de bits del mismo sample siempre será periódico junto al bytebeat.
    // Usamos un LFSR (polinomio x^16+x^15+x^13+x^4+1) sembrado con el sample
    // y perturbado cada vez para que sea independiente del contenido musical.
    // El resultado es ruido blanco no correlacionado con el bytebeat.
    uint16_t lfsr = macro_motion_.lfsr_state;
    if (lfsr == 0) lfsr = 0xACE1u;             // seed inicial si se fue a 0
    lfsr ^= (uint16_t)(u & 0xFFFFu);           // perturbar con bits bajos del sample
    // Galois LFSR — 1 paso
    const uint16_t lsb = lfsr & 1u;
    lfsr >>= 1;
    if (lsb) lfsr ^= 0xB400u;                  // polinomio Galois x^16+x^15+x^13+x^4+1
    macro_motion_.lfsr_state = lfsr;
    macro_motion_.chaos_raw = float(lfsr) * (1.0f / 65535.0f);

    // ── DENSITY: amplitud media de largo plazo ────────────────
    // Mide cuánto "material" hay en el bytebeat. Limitado al 60% para
    // evitar el loop de feedback positivo: drive alto → saturación →
    // density=1 → más drive. El cap rompe ese ciclo.
    const float abs_norm = clamp01(fabsf(float(bb_i)) * (1.0f / 32768.0f));
    macro_motion_.density_raw = abs_norm * 0.60f;

    // ── Tres EMA a velocidades distintas ─────────────────────
    // rhythm: rápido (fc ≈ 562 Hz) — sigue el pulso del bytebeat
    // chaos:  medio  (fc ≈ 140 Hz) — variación suave del LFSR
    // density: lento (fc ≈ 35 Hz)  — envolvente de amplitud
    macro_motion_.rhythm_s  += (macro_motion_.rhythm_raw  - macro_motion_.rhythm_s)  * 0.08f;
    macro_motion_.chaos_s   += (macro_motion_.chaos_raw   - macro_motion_.chaos_s)   * 0.02f;
    macro_motion_.density_s += (macro_motion_.density_raw - macro_motion_.density_s) * 0.005f;

    // ── Pan dinámico ──────────────────────────────────────────
    // rhythm_bi es bipolar (-1..+1), pan_depth controlado por macro.
    // EMA lento (fc ≈ 35 Hz) para evitar tremolo audible: el pan se
    // mueve como "respiración" estéreo, no como modulación de amplitud.
    const float rhythm_bi  = macro_motion_.rhythm_s * 2.0f - 1.0f;
    const float pan_depth  = 0.35f * m;
    const float pan_target = rhythm_bi * pan_depth;
    macro_motion_.pan_s += (pan_target - macro_motion_.pan_s) * 0.005f;  // era 0.02 → fc 35Hz

    // ── Gates secuenciales por macro ─────────────────────────
    // macro bajo → nada se modula
    // macro sube → drive, luego chorus, luego reverb, luego grain
    const float drive_gate  = smoothstep(0.20f, 0.70f, m);
    const float chorus_gate = smoothstep(0.35f, 0.75f, m);
    const float reverb_gate = smoothstep(0.50f, 1.00f, m);
    const float grain_gate  = smoothstep(0.75f, 1.00f, m);

    macro_out_.pan        = macro_motion_.pan_s;
    macro_out_.drive_mod  = macro_motion_.density_s * drive_gate  * 0.30f;
    macro_out_.chorus_mod = macro_motion_.chaos_s   * chorus_gate * 0.25f;
    macro_out_.reverb_mod = macro_motion_.density_s * reverb_gate * 0.25f;
    macro_out_.grain_mod  = macro_motion_.chaos_s   * grain_gate  * 0.35f;

    macro_motion_.prev_bb = bb_i;
}

void AudioEngine::refresh_control_block() {
    if (!state_mgr_) return;

    state_mgr_->fill_context(cached_ctx_);

    base_drive_ = state_mgr_->get_drive_live();
    base_reverb_room_ = state_mgr_->get_reverb_room();
    base_reverb_wet_ = state_mgr_->get_reverb_wet();
    base_chorus_ = state_mgr_->get_chorus_live();
    const float drive = clamp01(base_drive_ + macro_out_.drive_mod);
    const float reverb_room = base_reverb_room_;
    // Reverb wet: base + macro_mod + aftertouch (SNAP pads)
    // El aftertouch es aditivo: presión leve = poco espacio extra,
    // presión máxima = reverb al 100% independientemente del base.
    const float reverb_wet = clamp01(base_reverb_wet_ + macro_out_.reverb_mod + at_reverb_wet_);
    const float chorus = clamp01(base_chorus_ + macro_out_.chorus_mod);
    const float hp = state_mgr_->get_hp_live();
    base_grain_ = state_mgr_->get_grain_live();
    // Grain wet: base + macro_mod + aftertouch (MUTE pad)
    // El aftertouch de MUTE se suma al grain base. Si fx_freeze_on_,
    // el freeze ya corre en loop → el aftertouch suma wet adicional.
    const float grain = clamp01(base_grain_ + macro_out_.grain_mod + at_grain_wet_);
    const float snap = state_mgr_->get_snap_live();
    const float bpm = state_mgr_->get_encoder_state().bpm;
    const float stutter_rate = 0.25f + state_mgr_->get_stutter_rate_live() * 3.75f;
    const float env_attack = state_mgr_->get_env_attack();
    const float env_release = state_mgr_->get_env_release();
    const float env_loop_time = state_mgr_->get_env_loop_time();
    const float drum_color = state_mgr_->get_drum_color_live();
    const float drum_decay = state_mgr_->get_drum_decay_live();
    const float duck = state_mgr_->get_duck_amount_live();

    auto changed = [](float a, float b) -> bool { return (a < 0.0f) || (fabsf(a - b) > 0.0005f); };

    if (changed(cached_drive_, drive)) { dsp_.set_drive(drive); cached_drive_ = drive; }
    if (changed(cached_reverb_room_, reverb_room)) { dsp_.reverb().set_room_size(reverb_room); cached_reverb_room_ = reverb_room; }
    if (changed(cached_reverb_wet_, reverb_wet)) { dsp_.reverb().set_wet(reverb_wet); cached_reverb_wet_ = reverb_wet; }
    if (changed(cached_chorus_, chorus)) { dsp_.set_chorus_amount(chorus); cached_chorus_ = chorus; }
    if (changed(cached_hp_, hp)) { dsp_.set_hp_amount(hp); cached_hp_ = hp; }
    if (changed(cached_grain_, grain) && !fx_freeze_on_) { dsp_.set_grain_amount(grain); cached_grain_ = grain; }
    if (changed(cached_snap_, snap)) { dsp_.set_snap_amount(snap); cached_snap_ = snap; }
    if (changed(cached_bpm_for_snap_, bpm)) { dsp_.set_snap_bpm(bpm); cached_bpm_for_snap_ = bpm; }
    if (changed(cached_stutter_, stutter_rate)) { dsp_.stutter().set_rate(stutter_rate); cached_stutter_ = stutter_rate; }

    if (changed(cached_attack_, env_attack)) { envelope_.set_attack(env_attack); cached_attack_ = env_attack; }

    // FM/AM del lead: profundidad desde macro. Solo en Note Mode tiene
    // efecto audible (fuera de Note Mode el lead no se usa).
    // Se actualiza en control-rate para no llamar set_mod_depth cada sample.
    if (cached_ctx_.note_mode_active) {
        lead_osc_.set_mod_depth(cached_ctx_.macro);
    } else {
        lead_osc_.set_mod_depth(0.0f);  // seno puro fuera de Note Mode
    }
    if (changed(cached_release_, env_release)) { envelope_.set_release(env_release); cached_release_ = env_release; }
    envelope_.set_loop(state_mgr_->get_env_loop());
    if (changed(cached_env_loop_t_, env_loop_time)) { envelope_.set_loop_time_scale(env_loop_time); cached_env_loop_t_ = env_loop_time; }

    if (changed(cached_drum_color_, drum_color) || changed(cached_drum_decay_, drum_decay) || changed(cached_duck_, duck)) {
        drums_.set_params(drum_color, drum_decay, duck);
        cached_drum_color_ = drum_color;
        cached_drum_decay_ = drum_decay;
        cached_duck_ = duck;
    }

    quant_mix_q15_ = (cached_ctx_.note_mode_active ? 32767 : (int32_t)(cached_ctx_.quant_amount * 32767.0f));
    if (quant_mix_q15_ < 0) quant_mix_q15_ = 0;
    if (quant_mix_q15_ > 32767) quant_mix_q15_ = 32767;

    const float spread = cached_ctx_.spread;
    if (spread <= 0.01f) {
        spread_mix_q15_ = 0;
        spread_stride_ = 4;
    } else {
        // Mezcla máxima = 50% del canal R alternativo.
        spread_mix_q15_ = (uint16_t)(spread * 0.5f * 32767.0f);
        if (spread_mix_q15_ > 16384u) spread_mix_q15_ = 16384u;

        // A más spread, más resolución temporal. Reduce costo 2x-8x según uso.
        spread_stride_ = (spread > 0.66f) ? 2 : (spread > 0.33f ? 4 : 8);
        spread_phase_inc_q8_ = (uint8_t)(256 / spread_stride_);
        spread_t_offset_ = 1u + (uint32_t)(spread * 15.0f);
        spread_macro_delta_ = spread * 0.04f;
    }
}

void AudioEngine::begin_spread_segment(const EvalContext& ctx, uint32_t base_t, float pitch_ratio) {
    EvalContext ctx_now = ctx;
    ctx_now.t = (uint32_t)((float)(base_t + spread_t_offset_) * pitch_ratio);
    ctx_now.macro = ctx.macro + spread_macro_delta_;
    if (ctx_now.macro > 1.0f) ctx_now.macro = 1.0f;
    if (ctx_now.macro < 0.0f) ctx_now.macro = 0.0f;

    const uint32_t future_sample = sample_tick_ + spread_stride_;
    const uint32_t future_base_t = (uint32_t)((float)future_sample * ctx.time_div);
    EvalContext ctx_future = ctx_now;
    ctx_future.t = (uint32_t)((float)(future_base_t + spread_t_offset_) * pitch_ratio);

    spread_seg_start_ = state_mgr_->evaluate(ctx_now);
    spread_seg_end_   = state_mgr_->evaluate(ctx_future);
    spread_phase_     = 0;
}

void AudioEngine::drain_events() {
    if (!event_queue_) return;
    SequencerEvent ev;
    while (event_queue_->pop(ev)) {
        switch (ev.type) {

        case EVT_PARAM_CHANGE:
            switch (static_cast<ParamId>(ev.target)) {
            case PARAM_DRIVE:
                dsp_.set_drive(ev.value);
                break;
            case PARAM_SNAP_GATE:
                dsp_.set_snap_amount(ev.value);
                break;
            case PARAM_ENV_ATTACK:
                envelope_.set_attack(ev.value);
                break;
            case PARAM_ENV_RELEASE:
                envelope_.set_release(ev.value);
                break;
            case PARAM_GRAIN:
                if (!fx_freeze_on_) dsp_.set_grain_amount(ev.value);
                break;
            case PARAM_STUTTER_RATE:
                dsp_.stutter().set_rate(0.25f + ev.value * 3.75f);
                break;
            case PARAM_HP:
                dsp_.set_hp_amount(ev.value);
                break;
            case PARAM_REVERB_ROOM:
                dsp_.reverb().set_room_size(ev.value);
                break;
            case PARAM_REVERB_WET:
                dsp_.reverb().set_wet(ev.value);
                break;
            case PARAM_CHORUS:
                dsp_.set_chorus_amount(ev.value);
                break;
            default:
                break;
            }
            break;

        // V1.11: retrigger del envelope en cada trigger del sequencer
        // Esto permite que el release controle tick/pluck/pad en secuencias.
        // retrigger() NO resetea a 0 — el attack sube desde donde está
        // (legato natural, sin click en releases largos).
        case EVT_PAD_TRIGGER:
            env_gate_ = true;
            env_gate_hold_ctr_ = ENV_GATE_HOLD;  // armar el timer
            envelope_.retrigger();  // Note Mode lo cancela via EVT_NOTE_OFF si es necesario
            break;

        case EVT_DRUM_HIT:
            drums_.trigger((DrumId)ev.target);
            break;

        case EVT_ROLL_ON:
            drums_.roll_on((DrumId)ev.target);
            break;

        case EVT_ROLL_OFF:
            drums_.roll_off((DrumId)ev.target);
            break;

        case EVT_FX_ON:
            switch ((FxId)ev.target) {
            case FX_STUTTER:
                dsp_.stutter().gate_on();
                break;
            case FX_FREEZE:
                fx_freeze_on_ = true;
                dsp_.grain().force_freeze();
                break;
            case FX_OCT_DOWN:
                fx_oct_down_on_ = true;
                break;
            case FX_OCT_UP:
                fx_oct_up_on_ = true;
                break;
            case FX_VIBRATO:
                fx_vibrato_on_ = true;
                break;
            case FX_REPEAT_4:
                fx_repeat4_on_ = true;
                dsp_.stutter().set_rate(0.5f);
                dsp_.stutter().gate_repeat(1);
                break;
            case FX_REPEAT_8:
                fx_repeat8_on_ = true;
                dsp_.stutter().set_rate(1.0f);
                dsp_.stutter().gate_repeat(2);
                break;
            case FX_REPEAT_16:
                fx_repeat16_on_ = true;
                dsp_.stutter().set_rate(2.0f);
                dsp_.stutter().gate_repeat(4);
                break;
            }
            break;

        case EVT_FX_OFF:
            switch ((FxId)ev.target) {
            case FX_STUTTER:
                dsp_.stutter().gate_off();
                break;
            case FX_FREEZE:
                fx_freeze_on_ = false;
                dsp_.grain().force_release();
                if (state_mgr_) dsp_.set_grain_amount(state_mgr_->get_grain_live());
                else dsp_.set_grain_amount(0.0f);
                break;
            case FX_OCT_DOWN:
                fx_oct_down_on_ = false;
                break;
            case FX_OCT_UP:
                fx_oct_up_on_ = false;
                break;
            case FX_VIBRATO:
                fx_vibrato_on_ = false;
                break;
            case FX_REPEAT_4:
                fx_repeat4_on_ = false;
                if (!fx_repeat8_on_ && !fx_repeat16_on_) dsp_.stutter().gate_off();
                break;
            case FX_REPEAT_8:
                fx_repeat8_on_ = false;
                if (!fx_repeat4_on_ && !fx_repeat16_on_) dsp_.stutter().gate_off();
                break;
            case FX_REPEAT_16:
                fx_repeat16_on_ = false;
                if (!fx_repeat4_on_ && !fx_repeat8_on_) dsp_.stutter().gate_off();
                break;
            }
            break;

        // V1.7: drum params performáticos en vivo
        case EVT_DRUM_PARAM:
            if (ev.target == DRUM_PARAM_COLOR) {
                drums_.set_params(ev.value, -1.0f, -1.0f);  // solo color
            } else if (ev.target == DRUM_PARAM_DECAY) {
                drums_.set_params(-1.0f, ev.value, -1.0f);  // solo decay
            } else if (ev.target == DRUM_PARAM_DUCK) {
                drums_.set_params(-1.0f, -1.0f, ev.value);  // solo ducking
            }
            break;

        // V1.7: Note Mode — aplicar pitch ratio al bytebeat
        case EVT_NOTE_ON:
            if (state_mgr_) {
                float ratio = NoteMode::midi_to_pitch_ratio(ev.target);
                note_pitch_ratio_ = ratio;
            }
            env_gate_ = true;   // V1.10: note on → abrir gate del envelope
            break;

        case EVT_NOTE_OFF:
            note_pitch_ratio_ = 1.0f;
            env_gate_ = false;  // V1.10: note off → cerrar gate
            break;

        case EVT_AFTERTOUCH: {
            const float p   = ev.value;
            const uint8_t tgt = ev.target;

            if (tgt == PAD_MUTE) {
                // ── MUTE pad → grain freeze wet ──────────────────
                // La presión de la palma modula el wet del granulador.
                // Cuando fx_freeze_on_ está activo, el grain ya está
                // en loop; el aftertouch suma wet adicional para
                // profundizar el efecto de forma expresiva.
                at_grain_wet_ = p;
                // Si el freeze no estaba activo, activarlo suavemente
                // en cuanto la presión supera el umbral.
                if (p > 0.05f && !fx_freeze_on_) {
                    dsp_.set_grain_amount(clamp01(base_grain_ + p));
                } else if (p < 0.01f && !fx_freeze_on_) {
                    // Presión retirada sin freeze activo → restaurar base
                    dsp_.set_grain_amount(base_grain_);
                }
                // cached_grain_ se invalida para que refresh lo recalcule
                cached_grain_ = -1.0f;

            } else if (tgt == AT_TARGET_REVERB_SNAP) {
                // ── SNAP pads → reverb wet momentáneo ────────────
                // La presión abre el espacio: se suma al base_reverb_wet_
                // actual. Al soltar el pad (p=0.0), el reverb vuelve al base.
                // El cambio es suave porque EVT_AFTERTOUCH se emite con
                // hysteresis de 0.015 en handle_aftertouch.
                at_reverb_wet_ = p;
                // Invalidar cache para que refresh lo aplique inmediatamente
                cached_reverb_wet_ = -1.0f;

            } else {
                // Nota MIDI (Note Mode): target = número de nota MIDI.
                // La presión aumenta la profundidad FM momentáneamente
                // más allá del valor base del macro.
                // macro=0.3 + presión=1.0 → fm_depth sube hasta el máximo (0.40).
                // Permite expresividad táctil: tocar suave = seno limpio,
                // apretar = el bytebeat imprime su timbre en la nota.
                if (cached_ctx_.note_mode_active) {
                    const float base_depth = cached_ctx_.macro * 0.40f;
                    const float at_extra   = p * (0.40f - base_depth);
                    lead_osc_.set_mod_depth((base_depth + at_extra) / 0.40f);
                }
            }
            break;
        }

        default: break;
        }
    }
}

void AudioEngine::process_one_sample() {
    // Safe point cada BLOCK_SIZE samples: control-rate update.
    if ((sample_tick_ & (BLOCK_SIZE - 1)) == 0) {
        if (state_mgr_) state_mgr_->process_pending();
        drain_events();
        refresh_control_block();
    }

    if (!state_mgr_) { output_->write(0, 0); return; }

    // ── Bytebeat synth ───────────────────────────────────────
    EvalContext ctx = cached_ctx_;

    // V1.7: Note Mode — modular ctx.t con pitch ratio
    // ctx.t escalado = t * ratio (más agudo si ratio > 1)
    // Usamos nota activa del state si note_mode_active
    float pitch_ratio = ctx.note_pitch_ratio;  // 1.0 si no hay nota activa

    const float fx_pitch_target = fx_oct_down_on_ ? 0.5f : (fx_oct_up_on_ ? 2.0f : 1.0f);
    fx_pitch_mul_current_ += (fx_pitch_target - fx_pitch_mul_current_) * 0.08f;
    pitch_ratio *= fx_pitch_mul_current_;

    const float vibrato_target = fx_vibrato_on_ ? 1.0f : 0.0f;
    vibrato_depth_current_ += (vibrato_target - vibrato_depth_current_) * 0.04f;
    if (vibrato_depth_current_ > 0.001f) {
        extern const int16_t SINE_TABLE_256[256];
        const int16_t vib_s = SINE_TABLE_256[(uint8_t)(vibrato_phase_q24_ >> 16)];
        vibrato_phase_q24_ += VIBRATO_RATE_Q24;
        const float vib = 1.0f + ((float)vib_s / 32767.0f) * (0.018f + 0.01f * vibrato_depth_current_);
        pitch_ratio *= vib;
    }

    uint32_t base_t   = (uint32_t)((float)sample_tick_ * ctx.time_div);
    ctx.t = (uint32_t)((float)base_t * pitch_ratio);

    int16_t synth_l = state_mgr_->evaluate(ctx);
    int16_t synth_r = synth_l;

    update_macro_motion((int32_t)synth_l, ctx.macro);

    if (spread_mix_q15_ != 0) {
        // Spread optimizado: en vez de evaluar el graph del canal R en cada
        // sample, calculamos segmentos cortos y los interpolamos. El carácter
        // sigue siendo algorítmico, pero el costo cae fuerte cuando spread > 0.
        if (spread_phase_ == 0 || spread_phase_ >= spread_stride_) {
            begin_spread_segment(ctx, base_t, pitch_ratio);
        }

        const uint8_t frac_q8 = (uint8_t)(spread_phase_ * spread_phase_inc_q8_);
        const int16_t raw_r = lerp_i16(spread_seg_start_, spread_seg_end_, frac_q8);
        spread_phase_++;

        const int32_t wet = spread_mix_q15_;
        const int32_t dry = 32767 - wet;
        synth_r = (int16_t)((((int32_t)synth_l * dry) + ((int32_t)raw_r * wet)) >> 15);
    } else {
        spread_phase_ = 0;
    }

    // ── Lead tonal ────────────────────────────────────────────
    // En Note Mode: el pitch viene directo del pad, no del byte alto del bytebeat
    if ((note_update_ctr_++ & 31u) == 0) {
        uint8_t pitch_q;
        if (ctx.note_mode_active && pitch_ratio != 1.0f) {
            // Usar nota del pad directamente (ratio ya aplicado a ctx.t)
            // Reconvertir ratio a nota MIDI para el lead oscillator
            // ratio = freq/440 → freq = ratio*440
            float freq_hz = pitch_ratio * 440.0f;
            if (freq_hz < 20.0f)   freq_hz = 20.0f;
            if (freq_hz > 8000.0f) freq_hz = 8000.0f;
            lead_osc_.set_freq_slew(freq_hz);
        } else {
            // Modo normal: pitch desde bytebeat
            uint8_t pitch_raw = (uint8_t)(((uint32_t)(synth_l + 32768) >> 9) & 0x7F);
            ScaleId sid = (ScaleId)ctx.scale_id;
            pitch_q = Quantizer::quantize(pitch_raw, sid, ctx.root);
            if (pitch_q != last_lead_note_) {
                lead_osc_.set_freq_slew(Quantizer::note_to_freq(pitch_q));
                last_lead_note_ = pitch_q;
            }
        }
    }

    // FM/AM: alimentar el bytebeat del frame actual al lead oscillator.
    // Se llama justo antes de process() para que la modulación use
    // el sample del ciclo actual (latencia cero perceptible).
    // Fuera de Note Mode feed_bytebeat igual corre pero fm_depth_=0 así que
    // no tiene costo de audio — solo actualiza el slew de bb_norm_.
    lead_osc_.feed_bytebeat(synth_l);

    int16_t lead_s = (int16_t)(((int32_t)lead_osc_.process() * 20000) >> 15);
    int16_t synth_mix_l, synth_mix_r;
    if (quant_mix_q15_ <= 0) {
        synth_mix_l = synth_l;
        synth_mix_r = synth_r;
    } else if (quant_mix_q15_ >= 32767) {
        synth_mix_l = lead_s;
        synth_mix_r = lead_s;
    } else {
        const int32_t qa   = quant_mix_q15_;
        const int32_t qa_c = 32767 - qa;
        synth_mix_l = (int16_t)((((int32_t)synth_l * qa_c) + ((int32_t)lead_s * qa)) >> 15);
        synth_mix_r = (int16_t)((((int32_t)synth_r * qa_c) + ((int32_t)lead_s * qa)) >> 15);
    }

    synth_mix_l = LevelScaler::scale(synth_mix_l, LevelScaler::SYNTH_GAIN);
    synth_mix_r = LevelScaler::scale(synth_mix_r, LevelScaler::SYNTH_GAIN);

    // ── Envelope AR (V1.11) ───────────────────────────────────
    //
    // COEFICIENTES: se actualizan solo cuando cambian los pots
    // (ver handle_adc en InputRouter → EVT_PARAM_CHANGE → drain_events).
    // NO se recalculan cada sample — solo en drain_events().
    //
    // GATE:
    //   Note Mode  → env_gate_ controlado por EVT_NOTE_ON/OFF
    //   Modo normal → one-shot por EVT_PAD_TRIGGER (retrigger + gate timer)
    //                 El gate baja tras ENV_GATE_HOLD mínimo, luego
    //                 el release controla el decay.
    //
    // Decrementar el gate timer en modo normal (no Note Mode)
    if (!ctx.note_mode_active && env_gate_) {
        if (env_gate_hold_ctr_ > 0) {
            --env_gate_hold_ctr_;
        } else {
            env_gate_ = false;  // bajar gate → inicia release
        }
    }
    bool env_gate_final = ctx.note_mode_active ? env_gate_ : env_gate_;
    // next_gain() avanza el estado UNA SOLA VEZ → mismo gain para L y R
    int32_t env_gain = envelope_.next_gain(env_gate_final);
    synth_mix_l = ArEnvelope::apply(synth_mix_l, env_gain);
    synth_mix_r = ArEnvelope::apply(synth_mix_r, env_gain);

    // Macro Motion: paneo dinámico derivado del propio bytebeat.
    // Usa el mismo valor en positivo/negativo para L/R con slew previo para evitar aspereza.
    {
        float pan = macro_out_.pan;
        if (pan > 1.0f) pan = 1.0f;
        if (pan < -1.0f) pan = -1.0f;
        float pan_l = 1.0f - ((pan > 0.0f) ? pan : 0.0f);
        float pan_r = 1.0f + ((pan < 0.0f) ? pan : 0.0f);
        int32_t pl = (int32_t)(pan_l * 32767.0f);
        int32_t pr = (int32_t)(pan_r * 32767.0f);
        synth_mix_l = (int16_t)(((int32_t)synth_mix_l * pl) >> 15);
        synth_mix_r = (int16_t)(((int32_t)synth_mix_r * pr) >> 15);
    }

    // ── Drum engine + sidechain ──────────────────────────────
    int16_t drum_l = 0, drum_r = 0;
    int32_t sidechain_q15 = 32767;
    drums_.process(drum_l, drum_r, sidechain_q15);

    drum_l = LevelScaler::scale(drum_l, LevelScaler::DRUM_GAIN);
    drum_r = LevelScaler::scale(drum_r, LevelScaler::DRUM_GAIN);

    synth_mix_l = (int16_t)(((int32_t)synth_mix_l * sidechain_q15) >> 15);
    synth_mix_r = (int16_t)(((int32_t)synth_mix_r * sidechain_q15) >> 15);

    // ── Mix ──────────────────────────────────────────────────
    int32_t mix_l = (int32_t)synth_mix_l + drum_l;
    int32_t mix_r = (int32_t)synth_mix_r + drum_r;
    if (mix_l >  32767) mix_l =  32767;
    if (mix_l < -32768) mix_l = -32768;
    if (mix_r >  32767) mix_r =  32767;
    if (mix_r < -32768) mix_r = -32768;

    // ── DSP chain: stutter→dc→clip→reverb→limit ─────────────
    int16_t out_l = (int16_t)mix_l;
    int16_t out_r = (int16_t)mix_r;
    dsp_.process(out_l, out_r);
    output_->write(out_l, out_r);
}
