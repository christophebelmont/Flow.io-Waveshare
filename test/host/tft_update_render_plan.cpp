// g++ -std=c++17 -Wall -Wextra -Werror -pedantic -Isrc test/host/tft_update_render_plan.cpp -o /tmp/tft_render_plan && /tmp/tft_render_plan
#include "Modules/TFTModuleS3/TftUpdateRenderPlan.h"
#include <cassert>

int main() {
    using tft_update::renderDirty;
    const auto first = renderDirty(true, false, false, false, false, 0, 255);
    assert(first.full && first.title && first.progress && first.details);
    const auto idle = renderDirty(false, false, false, false, false, 10, 10);
    assert(!idle.full && !idle.title && !idle.progress && !idle.details);
    const auto one = renderDirty(false, false, false, false, false, 11, 10);
    assert(!one.progress && !one.details);
    const auto two = renderDirty(false, false, false, false, false, 12, 10);
    assert(two.progress && !two.title && !two.details);
    const auto complete = renderDirty(false, false, false, false, false, 100, 99);
    assert(complete.progress);
    const auto reset = renderDirty(false, false, false, false, false, 0, 99);
    assert(reset.progress);
    const auto step = renderDirty(false, true, false, false, false, 0, 78);
    assert(!step.title && step.progress && step.details);
    const auto state = renderDirty(false, false, true, false, false, 64, 64);
    assert(state.title && state.progress && state.details);
    const auto locale = renderDirty(false, false, false, false, true, 64, 64);
    assert(locale.title && locale.progress && locale.details);
    const auto target = renderDirty(false, false, false, true, false, 64, 64);
    assert(target.title && target.progress && target.details);
    return 0;
}
