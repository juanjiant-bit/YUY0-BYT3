// bytebeat_graph.cpp — Bytebeat Machine V2.0
#include "bytebeat_graph.h"
#include "../utils/debug_log.h"
#include <cstring>
#include <cstdlib>

uint8_t BytebeatGraph::compile_node(uint8_t node_idx, uint8_t* map) {
    if (node_idx == 255 || node_idx >= pool_size_) return 255;
    if (map[node_idx] != 255) return map[node_idx];

    const Node& n = pool_[node_idx];
    uint8_t a = 255;
    uint8_t b = 255;

    switch (n.type) {
    case NodeType::NEG:
    case NodeType::FOLD:
        a = compile_node(n.left, map);
        break;
    case NodeType::ADD:
    case NodeType::SUB:
    case NodeType::MUL:
    case NodeType::AND:
    case NodeType::OR:
    case NodeType::XOR:
    case NodeType::SHR:
    case NodeType::SHL:
    case NodeType::MOD:
    case NodeType::GT:
        a = compile_node(n.left, map);
        b = compile_node(n.right, map);
        break;
    default:
        break;
    }

    const uint8_t prog_idx = program_size_++;
    program_[prog_idx] = { n.type, n.const_val, a, b };
    map[node_idx] = prog_idx;
    return prog_idx;
}

void BytebeatGraph::compile_program() {
    program_size_ = 0;
    uint8_t map[MAX_NODES];
    memset(map, 0xFF, sizeof(map));
    if (pool_size_ == 0) {
        program_[0] = { NodeType::CONST, 0, 255, 255 };
        program_size_ = 1;
        return;
    }
    compile_node(root_, map);
}

int32_t BytebeatGraph::evaluate_raw_compiled(const EvalContext& ctx) const {
    int32_t values[MAX_NODES];
    for (uint8_t i = 0; i < program_size_; ++i) {
        const CompiledNode& n = program_[i];
        switch (n.type) {
        case NodeType::T:          values[i] = (int32_t)ctx.t; break;
        case NodeType::CONST:      values[i] = n.const_val; break;
        case NodeType::MACRO:      values[i] = 1 + (int32_t)(ctx.macro * 511.0f); break;
        case NodeType::ADD:        values[i] = values[n.a] + values[n.b]; break;
        case NodeType::SUB:        values[i] = values[n.a] - values[n.b]; break;
        case NodeType::MUL:        values[i] = values[n.a] * values[n.b]; break;
        case NodeType::AND:        values[i] = values[n.a] & values[n.b]; break;
        case NodeType::OR:         values[i] = values[n.a] | values[n.b]; break;
        case NodeType::XOR:        values[i] = values[n.a] ^ values[n.b]; break;
        case NodeType::NEG:        values[i] = -values[n.a]; break;
        case NodeType::SHR:        values[i] = (int32_t)((uint32_t)values[n.a] >> (values[n.b] & 0x1F)); break;
        case NodeType::SHL:        values[i] = values[n.a] << (values[n.b] & 0x1F); break;
        case NodeType::MOD:        values[i] = (values[n.b] != 0) ? (values[n.a] % values[n.b]) : 0; break;
        case NodeType::TONAL_NODE: values[i] = (int32_t)ctx.t * n.const_val; break;
        case NodeType::CLOCK_B:    values[i] = (int32_t)((uint32_t)ctx.t >> (uint32_t)n.const_val); break;
        case NodeType::FOLD: {
            uint8_t b = (uint8_t)(values[n.a] & 0xFF);
            values[i] = (int32_t)(b <= 127 ? b : 255 - b);
            break;
        }
        case NodeType::GT:         values[i] = (values[n.a] > values[n.b]) ? 255 : 0; break;
        default:                   values[i] = 0; break;
        }
    }
    return program_size_ ? values[program_size_ - 1] : 0;
}

static uint32_t lcg_next(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return s;
}
static uint8_t  lcg_u8(uint32_t& s) { return (uint8_t)(lcg_next(s) >> 24); }
static int32_t  lcg_range(uint32_t& s, int32_t lo, int32_t hi) {
    if (lo >= hi) return lo;
    return lo + (int32_t)(lcg_next(s) % (uint32_t)(hi - lo + 1));
}

void BytebeatGraph::generate(uint32_t seed, uint8_t zone, const ZoneConfig& cfg) {
    seed_          = seed;
    zone_          = zone;
    silence_count_ = 0;

    float   best_score = -1.0f;
    uint8_t best_root  = 0;
    Node    best_pool[MAX_NODES];
    uint8_t best_size  = 0;

    uint32_t rng = seed;

    for (uint8_t attempt = 0; attempt < QF_RETRIES; attempt++) {
        pool_size_ = 0;
        root_      = build_node(0, cfg.max_depth, rng, zone, cfg);
        compile_program();

        float score = evaluate_quality(rng);
        if (score > best_score) {
            best_score = score;
            best_root  = root_;
            best_size  = pool_size_;
            memcpy(best_pool, pool_, pool_size_ * sizeof(Node));
        }

        static constexpr float QF_ACCEPT_THRESHOLD = 0.08f;
        if (score >= QF_ACCEPT_THRESHOLD) {
            LOG("BB: seed=%lu zona=%u intento=%u score=%.3f OK",
                (unsigned long)seed, zone, attempt, (double)score);
            break;
        }

        rng ^= (rng << 13) ^ (attempt * 0x9E3779B9u);
    }

    root_          = best_root;
    pool_size_     = best_size;
    quality_score_ = best_score;
    memcpy(pool_, best_pool, best_size * sizeof(Node));
    compile_program();

    LOG("BB: seed=%lu zona=%u nodos=%u prog=%u score=%.3f",
        (unsigned long)seed, zone, pool_size_, program_size_, (double)quality_score_);
}

float BytebeatGraph::evaluate_quality(uint32_t /*seed_eval*/) const {
    EvalContext ctx{};
    ctx.macro            = 0.5f;
    ctx.tonal            = 0.0f;
    ctx.time_div         = 1.0f;
    ctx.zone             = zone_;
    ctx.note_pitch_ratio = 1.0f;
    ctx.note_mode_active = false;

    uint32_t dc_count  = 0;
    uint32_t sat_count = 0;
    uint32_t zc_count  = 0;
    uint16_t hist[16]  = {};
    int16_t prev = 0;

    for (uint16_t i = 0; i < QF_WINDOW; i++) {
        ctx.t = (uint32_t)i;
        int32_t raw = evaluate_raw_compiled(ctx);
        int16_t val = (int16_t)((raw & 0xFF) - 128);

        int16_t av = val < 0 ? -val : val;
        if (av < 16)  dc_count++;
        if (av > 110) sat_count++;
        if ((prev < 0 && val >= 0) || (prev >= 0 && val < 0)) zc_count++;
        prev = val;
        hist[(uint8_t)((raw & 0xFF) >> 4)]++;
    }

    float dc  = (float)dc_count  / QF_WINDOW;
    float sat = (float)sat_count / QF_WINDOW;
    float zc  = (float)zc_count  / (QF_WINDOW / 2);
    if (zc > 1.0f) zc = 1.0f;

    float entropy = 0.0f;
    for (uint8_t b = 0; b < 16; b++) {
        if (hist[b] == 0) continue;
        float p = (float)hist[b] / QF_WINDOW;
        entropy += p * (1.0f - p);
    }
    static constexpr float GINI_MAX = 15.0f / 16.0f;
    entropy /= GINI_MAX;
    if (entropy > 1.0f) entropy = 1.0f;

    float zc_factor = zc * 8.0f + 0.1f;
    if (zc_factor > 1.0f) zc_factor = 1.0f;

    float score = (1.0f - dc) * (1.0f - sat) * entropy * zc_factor;
    if (dc > QF_DC_MAX) score = 0.0f;
    if (sat > QF_SAT_MAX) score = 0.0f;
    if (entropy < QF_ENTROPY_MIN) score = 0.0f;
    if (zc < QF_ZC_MIN) score = 0.0f;
    return score;
}

uint8_t BytebeatGraph::build_node(uint8_t depth, uint8_t max_depth,
                                  uint32_t& rng, uint8_t /*zone*/,
                                  const ZoneConfig& cfg) {
    if (pool_size_ >= MAX_NODES) {
        uint8_t idx = pool_size_++;
        pool_[idx]  = { NodeType::T, 0, 255, 255 };
        return idx;
    }

    uint8_t idx = pool_size_++;
    Node&   n   = pool_[idx];
    n.left = n.right = 255;

    bool is_leaf = (depth >= max_depth) || (depth > 0 && lcg_u8(rng) < 60);
    if (is_leaf) {
        uint8_t r = lcg_u8(rng);
        if (r < cfg.macro_prob) {
            n.type = NodeType::MACRO;
            n.const_val = 0;
        } else if (r < 200) {
            bool musical = (lcg_u8(rng) < cfg.musical_const_prob);
            n.const_val = musical
                ? MUSICAL_CONSTS[lcg_next(rng) % MUSICAL_CONSTS_COUNT]
                : lcg_range(rng, cfg.const_min, cfg.const_max);
            n.type = NodeType::CONST;
        } else {
            n.type = NodeType::T;
            n.const_val = 0;
        }
        return idx;
    }

    NodeType op = cfg.ops[lcg_next(rng) % cfg.ops_count];
    n.type = op;

    if (op == NodeType::TONAL_NODE) {
        n.const_val = TONAL_CONSTS[lcg_next(rng) % TONAL_CONSTS_COUNT];
    } else if (op == NodeType::CLOCK_B) {
        n.const_val = 2;
    } else if (op == NodeType::FOLD || op == NodeType::NEG) {
        n.left = build_node(depth + 1, max_depth, rng, 0, cfg);
    } else if (op == NodeType::GT) {
        n.left  = build_node(depth + 1, max_depth, rng, 0, cfg);
        n.right = build_node(depth + 1, max_depth, rng, 0, cfg);
    } else {
        n.left  = build_node(depth + 1, max_depth, rng, 0, cfg);
        n.right = build_node(depth + 1, max_depth, rng, 0, cfg);
    }
    return idx;
}

int16_t BytebeatGraph::evaluate(const EvalContext& ctx) {
    int32_t raw = evaluate_raw_compiled(ctx);
    int16_t out = (int16_t)((raw & 0xFF) - 128);
    out = (int16_t)((int32_t)out * 256);

    if (ctx.tonal > 0.01f) {
        uint8_t bits_off = (uint8_t)(ctx.tonal * 7.0f);
        uint8_t mask     = (uint8_t)(0xFF << bits_off);
        int32_t raw_masked = (int32_t)((uint8_t)((out >> 8) + 128) & mask) - 128;
        out = (int16_t)(raw_masked * 256);
    }

    if (out == 0 || (out > -64 && out < 64)) {
        if (++silence_count_ > SILENCE_THRESHOLD) {
            noise_state_ ^= (uint16_t)(noise_state_ << 7);
            noise_state_ ^= (uint16_t)(noise_state_ >> 9);
            noise_state_ ^= (uint16_t)(noise_state_ << 8);
            out = (int16_t)((int16_t)(noise_state_ & 0x3FF) - 512);
            if (silence_count_ > SILENCE_THRESHOLD + 2205) silence_count_ = 0;
        }
    } else {
        silence_count_ = 0;
    }

    return out;
}

static const char* node_name(NodeType t) {
    switch (t) {
    case NodeType::T:          return "T";
    case NodeType::CONST:      return "CONST";
    case NodeType::MACRO:      return "MACRO";
    case NodeType::ADD:        return "ADD";
    case NodeType::SUB:        return "SUB";
    case NodeType::MUL:        return "MUL";
    case NodeType::AND:        return "AND";
    case NodeType::OR:         return "OR";
    case NodeType::XOR:        return "XOR";
    case NodeType::SHR:        return "SHR";
    case NodeType::SHL:        return "SHL";
    case NodeType::MOD:        return "MOD";
    case NodeType::NEG:        return "NEG";
    case NodeType::TONAL_NODE: return "TONAL";
    case NodeType::CLOCK_B:    return "CLKB";
    case NodeType::FOLD:       return "FOLD";
    case NodeType::GT:         return "GT";
    default:                   return "???";
    }
}

void BytebeatGraph::debug_print() const {
    LOG("BB árbol: %u nodos raiz=%u prog=%u score=%.3f",
        pool_size_, root_, program_size_, (double)quality_score_);
    for (uint8_t i = 0; i < pool_size_; i++) {
        const Node& n = pool_[i];
        if (n.type == NodeType::CONST || n.type == NodeType::TONAL_NODE || n.type == NodeType::CLOCK_B)
            LOG("  [%2u] %s val=%ld", i, node_name(n.type), (long)n.const_val);
        else if (n.left == 255)
            LOG("  [%2u] %s", i, node_name(n.type));
        else
            LOG("  [%2u] %s L=%u R=%u", i, node_name(n.type), n.left, n.right);
    }
}
