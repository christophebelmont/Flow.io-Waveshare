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
    const GFXfont* font = nullptr;
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
    // Upper-area contract: header 48 px; IP and URL stay exactly at their existing positions.
    assert(kHeaderH == 48);
    assert(sameRect(overviewTopCardRect_(320,0),{8,56,149,34}));
    assert(sameRect(overviewTopCardRect_(320,1),{163,56,149,34}));
    for (uint8_t mode = 0; mode < 3; ++mode) for (uint8_t ph = 0; ph < 3; ++ph) for (uint8_t treatment = 0; treatment < 4; ++treatment) {
        TFTModuleS3 module;
        TFTModuleS3::OverviewStatusRenderState state;
        state.operatingAvailable = state.treatmentAvailable = true;
        state.operatingMode = PoolOperatingMode(mode);
        state.phCorrection = PoolPhCorrection(ph);
        state.treatment = PoolDisinfectionMethod(treatment);
        module.drawOverviewSelectors_(state);
        const auto& display = module.display_;
        assert(display.fills.size() == 10 && display.outlines.size() == 10 && display.texts.size() == 10);
        const std::vector<std::string> expected = {"Manuel","Auto","Hiver","pH+","pH-","Off","Chlore","Electro","O2","Off"};
        unsigned activeByRow[3]{};
        for (unsigned i = 0; i < display.texts.size(); ++i) {
            const auto& rect = display.fills[i].rect;
            const auto& text = display.texts[i];
            const unsigned row = i < 3 ? 0 : i < 6 ? 1 : 2;
            assert(text.value == expected[i]);
            assert(text.font == &FreeSans9pt7b);
            assert(rect.x >= 8 && rect.x + rect.w <= 312);
            assert(rect.y >= 98 && rect.y + rect.h <= 232);
            assert(rect.h >= 40);
            assert(sameRect(rect,display.outlines[i].rect));
            assert(text.ink.x >= rect.x + 6 && text.ink.x + text.ink.w <= rect.x + rect.w - 6);
            assert(text.ink.y >= rect.y + 6 && text.ink.y + text.ink.h <= rect.y + rect.h - 6);
            const int centerErrorX = 2*(text.ink.x - rect.x) + text.ink.w - rect.w;
            const int centerErrorY = 2*(text.ink.y - rect.y) + text.ink.h - rect.h;
            assert(centerErrorX >= -1 && centerErrorX <= 0);
            assert(centerErrorY >= -1 && centerErrorY <= 0);
            if (display.fills[i].color == kColorSelected) {
                ++activeByRow[row];
                assert(text.foreground == kColorBg && text.background == kColorSelected);
            } else assert(text.foreground == kColorSelected && text.background == kColorCardBg);
            for (unsigned j = i+1; j < display.fills.size(); ++j) {
                const auto& other = display.fills[j].rect;
                assert(rect.x + rect.w <= other.x || other.x + other.w <= rect.x ||
                       rect.y + rect.h <= other.y || other.y + other.h <= rect.y);
            }
        }
        assert(activeByRow[0] == 1 && activeByRow[1] == 1 && activeByRow[2] == 1);
    }
    TFTModuleS3 unknown;
    unknown.drawOverviewSelectors_({});
    for (const auto& fill : unknown.display_.fills) assert(fill.color != kColorSelected);
    for (const auto& text : unknown.display_.texts) assert(text.foreground == kColorMuted);
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

int main() {
    nativeGeometryAndFont();
    configuredValuesAndCacheInvalidation();
    puts("TFT overview: 36 configurations, native 320x240 bounds, actual FreeSans9pt glyphs, configured states and repaint invalidation OK");
}
