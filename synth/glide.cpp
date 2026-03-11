// glide.cpp
#include "glide.h"

void Glide::start(BytebeatGraph* from, BytebeatGraph* to,
                  uint32_t duration_samples) {
    from_     = from;
    to_       = to;
    duration_ = duration_samples;
    position_ = 0;
    active_   = (duration_samples > 0 && from && to);
}

int16_t Glide::evaluate(const EvalContext& ctx) {
    if (!active_ || !to_)
        return to_ ? to_->evaluate(ctx) : 0;

    int16_t s_from = from_->evaluate(ctx);
    int16_t s_to   = to_->evaluate(ctx);

    float   alpha = (float)position_ / (float)duration_;
    int16_t out   = (int16_t)((1.0f - alpha) * s_from + alpha * s_to);

    if (++position_ >= duration_)
        active_ = false;

    return out;
}
