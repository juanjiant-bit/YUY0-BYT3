#pragma once
// glide.h — crossfade lineal entre dos BytebeatGraphs
#include "bytebeat_graph.h"

class Glide {
public:
    void    start(BytebeatGraph* from, BytebeatGraph* to,
                  uint32_t duration_samples);
    int16_t evaluate(const EvalContext& ctx);
    bool    is_active() const { return active_; }

private:
    BytebeatGraph* from_     = nullptr;
    BytebeatGraph* to_       = nullptr;
    uint32_t       duration_ = 0;
    uint32_t       position_ = 0;
    bool           active_   = false;
};
