#pragma once
// bytebeat_graph.h — Bytebeat Machine V2.0
// CAMBIOS RESPECTO A V1.x:
//
//   + Filtro de calidad automático en generate():
//       Evalúa el árbol por QF_WINDOW samples y mide 5 métricas:
//         1. dc_ratio    — fracción del tiempo en zona de silencio (<64)
//         2. sat_ratio   — fracción del tiempo saturado (>200 o <-200 de 256)
//         3. entropy     — dispersión del byte bajo en 16 cubetas (0=DC, 1=blanco)
//         4. period_score— estimación de periodicidad mínima (detecta DC puro)
//         5. zero_cross  — tasa de cruces por cero normalizada
//       Si el árbol no pasa QF_RETRIES intentos, se acepta el mejor hasta ahora.
//
//   + NodeType: agregados TONAL_NODE y CLOCK_B
//       TONAL_NODE — multiplica t por una constante musical grande (pitch hint)
//       CLOCK_B    — t derivado lento (t >> 2), enriquece ritmos internos
//
//   + bytebeat_node.h: MAX_NODES 31 → 48 para árboles más ricos
//       (cabe en el pool estático de 48 × 5 bytes = 240 bytes, no hay heap)
//
//   + generate() expone get_quality_score() → float 0-1 del último árbol aceptado
//
//   + Compatibilidad total con la API existente: misma firma, mismos includes.
//
#include "bytebeat_node.h"
#include "zone_config.h"

class BytebeatGraph {
public:
    // API idéntica a V1: drop-in replacement
    void    generate(uint32_t seed, uint8_t zone, const ZoneConfig& cfg);
    int16_t evaluate(const EvalContext& ctx);

    uint32_t get_seed()          const { return seed_; }
    uint8_t  get_zone()          const { return zone_; }
    float    get_quality_score() const { return quality_score_; }
    void     debug_print()       const;

    // ── Parámetros del filtro de calidad ─────────────────────
    // Ajustables sin recompilar si los exponés como constantes del proyecto.
    // Valores conservadores que rechazan los peores árboles sin ser demasiado
    // exigentes (importante: el generador sigue siendo rápido en el peor caso).
    static constexpr uint16_t QF_WINDOW  = 512;   // samples a evaluar por intento
    static constexpr uint8_t  QF_RETRIES = 6;     // reintentos máximos antes de
                                                   // aceptar el mejor disponible
    // Umbrales de rechazo (ver evaluate_quality() para interpretación):
    static constexpr float QF_DC_MAX     = 0.85f; // rechazar si >85% del tiempo en silencio
    static constexpr float QF_SAT_MAX    = 0.90f; // rechazar si >90% del tiempo saturado
    static constexpr float QF_ENTROPY_MIN= 0.10f; // rechazar si entropía < 10% del máximo
    static constexpr float QF_ZC_MIN     = 0.005f;// rechazar si casi nunca cruza 0

private:
    struct CompiledNode {
        NodeType type;
        int32_t  const_val;
        uint8_t  a;
        uint8_t  b;
    };

    uint8_t build_node(uint8_t depth, uint8_t max_depth,
                       uint32_t& rng, uint8_t zone,
                       const ZoneConfig& cfg);
    float   evaluate_quality(uint32_t seed_eval) const;
    int32_t evaluate_raw_compiled(const EvalContext& ctx) const;
    uint8_t compile_node(uint8_t node_idx, uint8_t* map);
    void    compile_program();

    Node     pool_[MAX_NODES];
    uint8_t  pool_size_ = 0;
    uint8_t  root_      = 0;
    uint32_t seed_      = 0;
    uint8_t  zone_      = 0;
    float    quality_score_ = 0.0f;

    // Programa compilado en postorden. Evita recursión en el hot path y
    // deja el runtime del graph en una pasada lineal por sample.
    CompiledNode program_[MAX_NODES] = {};
    uint8_t      program_size_ = 0;

    // Anti-silencio (igual que V1, como red de seguridad de último recurso)
    uint32_t silence_count_ = 0;
    static constexpr uint32_t SILENCE_THRESHOLD = 2205;
    uint16_t noise_state_   = 0xACE1u;
};
