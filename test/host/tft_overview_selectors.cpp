#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "Core/Services/IPoolConfiguration.h"

#define PROGMEM
// FONT_TYPES
// FONT_DATA
// PRODUCTION_CONSTANTS
// PRODUCTION_GEOMETRY

struct FakeDisplay {
    struct Paint { Rect rect; uint16_t color; };
    struct Text { std::string value; Rect ink; const GFXfont* font; uint16_t foreground, background; };
    std::vector<Paint> fills, outlines;
    std::vector<Text> texts;
    std::vector<std::string> captions;
    const GFXfont* font = nullptr;
    void setFont(const GFXfont* f) { font = f; }
    void setTextSize(int) {}
    void setTextColor(uint16_t, uint16_t) {}
    void setCursor(int16_t, int16_t) {}
    void drawFastVLine(int16_t, int16_t, int16_t, uint16_t) {}
    void drawFastHLine(int16_t, int16_t, int16_t, uint16_t) {}
    void drawCircle(int16_t, int16_t, int16_t, uint16_t) {}
    void fillCircle(int16_t, int16_t, int16_t, uint16_t) {}
    void drawLine(int16_t, int16_t, int16_t, int16_t, uint16_t) {}
    void fillTriangle(int16_t, int16_t, int16_t, int16_t, int16_t, int16_t, uint16_t) {}
    void print(const char* label) { captions.emplace_back(label ? label : ""); }
    int16_t width() const { return 320; }
    int16_t height() const { return 240; }
    void fillRoundRect(int16_t x,int16_t y,int16_t w,int16_t h,int16_t,uint16_t color) { fills.push_back({{x,y,w,h},color}); }
    void drawRoundRect(int16_t x,int16_t y,int16_t w,int16_t h,int16_t,uint16_t color) { outlines.push_back({{x,y,w,h},color}); }
    void getTextBounds(const char* text,int16_t x,int16_t y,int16_t* x1,int16_t* y1,uint16_t* w,uint16_t* h) {
        assert(font == &FreeSans9pt7b);
        int16_t left = INT16_MAX, top = INT16_MAX, right = -1, bottom = -1;
        for (const unsigned char* c = reinterpret_cast<const unsigned char*>(text); *c; ++c) {
            assert(*c >= font->first && *c <= font->last);
            const auto& glyph = font->glyph[*c - font->first];
            const int16_t gx = x + glyph.xOffset, gy = y + glyph.yOffset;
            left = std::min(left,gx); top = std::min(top,gy);
            right = std::max<int16_t>(right,gx + glyph.width - 1);
            bottom = std::max<int16_t>(bottom,gy + glyph.height - 1);
            x += glyph.xAdvance;
        }
        *x1 = left; *y1 = top; *w = right-left+1; *h = bottom-top+1;
    }
};

class TFTModuleS3 {
public:
    // PRODUCTION_STATE
    // PRODUCTION_CHOICE
    FakeDisplay display_;
    const PoolConfigurationService* poolConfigurationSvc_ = nullptr;
    uint16_t color_(uint16_t color) const { return color; }
    void setGfxFont_(const GFXfont* font, uint16_t, uint16_t) { display_.font = font; }
    void drawGfxText_(const GFXfont* font,uint16_t fg,uint16_t bg,int16_t x,int16_t baseline,const char* text) {
        display_.font = font;
        int16_t x1,y1; uint16_t w,h;
        display_.getTextBounds(text,x,baseline,&x1,&y1,&w,&h);
        display_.texts.push_back({text,{x1,y1,int16_t(w),int16_t(h)},font,fg,bg});
    }
    void drawMiniIcon_(int16_t, int16_t, uint8_t, uint16_t) {}
    void drawOverviewSelectorRow_(uint8_t,const OverviewChoice*,uint8_t,uint8_t,bool);
    void drawOverviewSelectors_(const OverviewStatusRenderState&);
    static bool overviewStatusEquals_(const OverviewStatusRenderState&,const OverviewStatusRenderState&);
    void readOverviewStatus_(OverviewStatusRenderState&) const;
};
// PRODUCTION_METHODS

static bool sameRect(const Rect& a, const Rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

static void nativeGeometryAndFont()
{
    // Approved native 1px edges, 2px gaps, 32px common header.
    assert(kHeaderH == 32 && kCardEdge == 1 && kCardGap == 2);
    assert(sameRect(overviewTopCardRect_(320,0),{1,33,158,41}));
    assert(sameRect(overviewTopCardRect_(320,1),{161,33,158,41}));
    assert(sameRect(overviewStatusCardRect_(320,240,0U),{1,76,318,53}));
    assert(sameRect(overviewStatusCardRect_(320,240,1U),{1,131,318,53}));
    assert(sameRect(overviewStatusCardRect_(320,240,2U),{1,186,318,53}));

    static constexpr const char* modes[] = {"Manuel","Auto","Hiver"};
    static constexpr const char* ph[] = {"pH+","pH-","Off"};
    static constexpr const char* treatments[] = {"Chlore","Electro","O2","Off"};
    static constexpr const char* labels[] = {"Mode","Correction pH","Traitement"};
    static constexpr uint16_t colors[] = {
        kColorOverviewModeBg, kColorOverviewPhBg, kColorOverviewTreatmentBg
    };

    for (uint8_t mode = 0; mode < 3; ++mode)
    for (uint8_t correction = 0; correction < 3; ++correction)
    for (uint8_t treatment = 0; treatment < 4; ++treatment) {
        TFTModuleS3 module;
        TFTModuleS3::OverviewStatusRenderState state;
        state.operatingAvailable = state.treatmentAvailable = true;
        state.operatingMode = PoolOperatingMode(mode);
        state.phCorrection = PoolPhCorrection(correction);
        state.treatment = PoolDisinfectionMethod(treatment);
        module.drawOverviewSelectors_(state);
        const auto& d = module.display_;
        assert(d.fills.size() == 3 && d.outlines.size() == 3);
        assert(d.texts.size() == 6); // One label + selected value, not all options.
        const char* values[] = {modes[mode], ph[correction], treatments[treatment]};
        for (unsigned row = 0; row < 3; ++row) {
            assert(sameRect(d.fills[row].rect, overviewStatusCardRect_(320,240,(uint8_t)row)));
            assert(sameRect(d.fills[row].rect, d.outlines[row].rect));
            assert(d.fills[row].color == colors[row]);
            const auto& label = d.texts[row * 2U];
            const auto& selected = d.texts[row * 2U + 1U];
            assert(label.value == labels[row]);
            assert(label.font == &FreeSans9pt7b);
            assert(label.foreground == kColorText);
            assert(selected.value == values[row]);
            assert(selected.font == &FreeSans9pt7b);
            assert(selected.foreground == kColorText);
            const Rect card = d.fills[row].rect;
            assert(label.ink.x >= card.x && label.ink.x + label.ink.w < card.x + card.w);
            assert(selected.ink.x > label.ink.x + label.ink.w);
            assert(selected.ink.x + selected.ink.w <= card.x + card.w);
            assert(selected.ink.y >= card.y && selected.ink.y + selected.ink.h <= card.y + card.h);
        }
    }

    TFTModuleS3 unknown;
    unknown.drawOverviewSelectors_({});
    assert(unknown.display_.fills.size() == 3);
    assert(unknown.display_.texts.size() == 6);
    for (unsigned row = 0; row < 3; ++row) {
        const auto& v = unknown.display_.texts[row * 2U + 1U];
        assert(v.value == "--" && v.foreground == kColorMuted);
    }
}

struct PoolSource {
    PoolOperatingConfiguration operating;
    PoolCharacteristics characteristics;
    PoolConfigurationService service{};
    PoolSource() {
        service.ctx = this;
        service.getOperatingConfiguration = [](void* ctx, PoolOperatingConfiguration* out) {
            *out = static_cast<PoolSource*>(ctx)->operating; return true;
        };
        service.getCharacteristics = [](void* ctx, PoolCharacteristics* out) {
            *out = static_cast<PoolSource*>(ctx)->characteristics; return true;
        };
    }
};

static void configuredValuesAndCacheInvalidation()
{
    PoolSource source;
    source.operating.available = source.characteristics.available = true;
    source.operating.operatingMode = PoolOperatingMode::Manual;
    source.operating.phCorrection = PoolPhCorrection::Plus;
    source.characteristics.disinfectionMethod = PoolDisinfectionMethod::SaltElectrolysis;
    // Effective regulation flags stay false in Manu; display must use configured values.
    TFTModuleS3 module;
    module.poolConfigurationSvc_ = &source.service;
    TFTModuleS3::OverviewStatusRenderState first, second;
    module.readOverviewStatus_(first);
    assert(first.operatingAvailable && first.treatmentAvailable);
    assert(first.operatingMode == PoolOperatingMode::Manual);
    assert(first.phCorrection == PoolPhCorrection::Plus);
    assert(first.treatment == PoolDisinfectionMethod::SaltElectrolysis);
    module.readOverviewStatus_(second);
    assert(TFTModuleS3::overviewStatusEquals_(first,second));
    source.operating.phCorrection = PoolPhCorrection::Minus;
    module.readOverviewStatus_(second);
    assert(!TFTModuleS3::overviewStatusEquals_(first,second));
    source.operating.phCorrection = PoolPhCorrection::Plus;
    source.characteristics.disinfectionMethod = PoolDisinfectionMethod::ActiveOxygen;
    module.readOverviewStatus_(second);
    assert(!TFTModuleS3::overviewStatusEquals_(first,second));
    source.characteristics.disinfectionMethod = PoolDisinfectionMethod::SaltElectrolysis;
    source.operating.operatingMode = PoolOperatingMode::Winter;
    module.readOverviewStatus_(second);
    assert(!TFTModuleS3::overviewStatusEquals_(first,second));
    source.operating.available = false;
    module.readOverviewStatus_(second);
    assert(!second.operatingAvailable && second.treatmentAvailable);
    source.operating.available = true;
    source.operating.phCorrection = PoolPhCorrection(255);
    source.characteristics.disinfectionMethod = PoolDisinfectionMethod(255);
    module.readOverviewStatus_(second);
    assert(!second.operatingAvailable && !second.treatmentAvailable);
    module.poolConfigurationSvc_ = nullptr;
    module.readOverviewStatus_(second);
    assert(!second.operatingAvailable && !second.treatmentAvailable);
}

static void metricsAndAlarmsGridBounds()
{
    // Page 2: 10 live metrics in 5x2 (39px, last 40px).
    // Page 3: 8 alarm slots in 4x2 (50px); 8th may be disabled.
    for (const bool dense : {false, true}) {
        const unsigned total = dense ? 10U : 8U;
        std::vector<Rect> cards;
        for (unsigned i = 0; i < total; ++i) {
            const Rect r = metricCardRect_(320, 240, static_cast<uint8_t>(i), dense);
            assert(r.x >= 1 && r.x + r.w <= 319);
            assert(r.y >= 33 && r.y + r.h <= 239);
            assert(r.w == 158 && r.h >= (dense ? 39 : 50));
            for (const Rect& prior : cards) {
                assert(r.x + r.w <= prior.x || prior.x + prior.w <= r.x ||
                       r.y + r.h <= prior.y || prior.y + prior.h <= r.y);
            }
            cards.push_back(r);
        }
        assert(cards[0].y == cards[1].y && cards[0].x == 1 && cards[1].x == 161);
        assert(cards[total - 2].y == cards[total - 1].y);
        assert(cards[total - 1].y + cards[total - 1].h == 239);
        assert(cards[0].y == (dense ? 35 : 33));
    }
}

int main() {
    nativeGeometryAndFont();
    configuredValuesAndCacheInvalidation();
    metricsAndAlarmsGridBounds();
    puts("TFT dark: 36 selected-only modes, native 5x2 measurements, 4x2 alarms and 320x240 bounds OK");
}
