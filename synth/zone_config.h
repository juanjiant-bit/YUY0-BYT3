#pragma once
// zone_config.h — Bytebeat Machine V2.0
// CAMBIOS:
//   + NodeType::TONAL_NODE, CLOCK_B, FOLD, GT disponibles en zonas 0-4
//   + Cada zona tiene una paleta más descriptiva de su intención sonora
//
// FILOSOFÍA DE ZONAS (sin cambios en el concepto):
//   0 = melódico — máximas constantes musicales, mínimo caos
//   1 = semi-melódico
//   2 = neutral (default)
//   3 = semi-caos
//   4 = caos total
//
#include "bytebeat_node.h"
#include <cstdint>

struct ZoneConfig {
    NodeType ops[16];    // aumentado a 16 para más paleta
    uint8_t  ops_count;
    int32_t  const_min;
    int32_t  const_max;
    uint8_t  musical_const_prob;
    uint8_t  max_depth;
    uint8_t  macro_prob;
    // V2: probabilidades de los nodos nuevos (0-255 relativo al ops pool)
    // Se usan DENTRO de la paleta de ops — no son pesos separados.
    // Un nodo nuevo se incluye N veces en ops[] para aumentar su probabilidad.
};

static constexpr int32_t MUSICAL_CONSTS[] = {
    1, 2, 3, 4, 6, 8, 12, 16, 24, 32,
    48, 64, 96, 128, 192, 256, 384, 512
};
static constexpr uint8_t MUSICAL_CONSTS_COUNT = 18;

// Constantes TONAL_NODE: más grandes que las normales para dar pitch útil
static constexpr int32_t TONAL_CONSTS[] = {
    3, 5, 6, 7, 9, 10, 12, 14, 15, 18, 20, 21
};
static constexpr uint8_t TONAL_CONSTS_COUNT = 12;

inline ZoneConfig make_zone(uint8_t z) {
    ZoneConfig c{};
    switch (z) {

    case 0: // ── melódico ─────────────────────────────────────────────
        // Paleta: AND, SHR, SHL + TONAL_NODE (x2), FOLD (x1) para suavidad
        // Profundidad limitada → fórmulas más repetibles y afinadas
        c.ops[0]=NodeType::AND;
        c.ops[1]=NodeType::OR;
        c.ops[2]=NodeType::SHR;
        c.ops[3]=NodeType::SHL;
        c.ops[4]=NodeType::TONAL_NODE;  // énfasis en nodo tonal
        c.ops[5]=NodeType::TONAL_NODE;  // x2 → +probabilidad
        c.ops[6]=NodeType::FOLD;        // suaviza el byte-serrucho
        c.ops_count=7;
        c.const_min=1; c.const_max=512;
        c.musical_const_prob=240; c.max_depth=3; c.macro_prob=200;
        break;

    case 1: // ── semi-melódico ─────────────────────────────────────────
        c.ops[0]=NodeType::AND;
        c.ops[1]=NodeType::OR;
        c.ops[2]=NodeType::SHR;
        c.ops[3]=NodeType::SHL;
        c.ops[4]=NodeType::XOR;
        c.ops[5]=NodeType::MOD;
        c.ops[6]=NodeType::TONAL_NODE;
        c.ops[7]=NodeType::CLOCK_B;    // clock lento para base
        c.ops[8]=NodeType::FOLD;
        c.ops_count=9;
        c.const_min=1; c.const_max=1024;
        c.musical_const_prob=160; c.max_depth=3; c.macro_prob=150;
        break;

    case 2: // ── neutral (default) ─────────────────────────────────────
        // Buena mezcla de todo — es el punto de referencia
        c.ops[0]=NodeType::AND;
        c.ops[1]=NodeType::OR;
        c.ops[2]=NodeType::XOR;
        c.ops[3]=NodeType::SHR;
        c.ops[4]=NodeType::SHL;
        c.ops[5]=NodeType::ADD;
        c.ops[6]=NodeType::MOD;
        c.ops[7]=NodeType::TONAL_NODE;
        c.ops[8]=NodeType::CLOCK_B;
        c.ops[9]=NodeType::FOLD;
        c.ops[10]=NodeType::GT;        // pulso digital moderado
        c.ops_count=11;
        c.const_min=1; c.const_max=2048;
        c.musical_const_prob=100; c.max_depth=4; c.macro_prob=110;
        break;

    case 3: // ── semi-caos ─────────────────────────────────────────────
        c.ops[0]=NodeType::ADD;
        c.ops[1]=NodeType::SUB;
        c.ops[2]=NodeType::MUL;
        c.ops[3]=NodeType::AND;
        c.ops[4]=NodeType::XOR;
        c.ops[5]=NodeType::SHR;
        c.ops[6]=NodeType::SHL;
        c.ops[7]=NodeType::MOD;
        c.ops[8]=NodeType::CLOCK_B;
        c.ops[9]=NodeType::GT;         // gates más pronunciados
        c.ops[10]=NodeType::FOLD;
        c.ops_count=11;
        c.const_min=1; c.const_max=4096;
        c.musical_const_prob=50; c.max_depth=4; c.macro_prob=80;
        break;

    default: // ── caos total ────────────────────────────────────────────
        c.ops[0]=NodeType::ADD;
        c.ops[1]=NodeType::SUB;
        c.ops[2]=NodeType::MUL;
        c.ops[3]=NodeType::AND;
        c.ops[4]=NodeType::OR;
        c.ops[5]=NodeType::XOR;
        c.ops[6]=NodeType::SHR;
        c.ops[7]=NodeType::SHL;
        c.ops[8]=NodeType::MOD;
        c.ops[9]=NodeType::CLOCK_B;
        c.ops[10]=NodeType::GT;
        c.ops_count=11;
        c.const_min=1; c.const_max=65536;
        c.musical_const_prob=0; c.max_depth=4; c.macro_prob=60;
        break;
    }
    return c;
}
