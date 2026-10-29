#pragma once
// Colours of the device UI (docs/design/README.md, "设备主题"). Pure C++: no LVGL, so the rules can be tested on the host.
#include <array>
#include <cstdint>
#include <string>

namespace dh::ui {
enum class ThemeId { Mint, Ocean, Amber };
// 0xRRGGBB. `accent` and `selected` are the two colours a custom accent changes; the rest belongs to the theme.
struct Palette {
    uint32_t bg, bg2, card, edge, ink, muted, accent, selected, track, wifi, agent, power, clock, danger;
};
const Palette &theme_palette(ThemeId);
ThemeId theme_from_name(const std::string &name);  // anything unknown is Mint
const char *theme_name(ThemeId);                   // "mint", "ocean", "amber"

// The battery icon is filled with the colour of its level at this strength (percent) and the digits are drawn over the
// fill in ink. 40 keeps the digits at 4.5:1 or better in every theme (tested in ui_model_test.cpp, T20).
constexpr int kBatteryFillPercent = 40;
// `top` over `under`, `percent` (0..100) of `top`, per channel, rounded half up. 0xRRGGBB.
uint32_t blend(uint32_t top, uint32_t under, int percent);

// WCAG contrast ratio of two colours: 1 (same) to 21 (black on white). Symmetric.
double contrast_ratio(uint32_t a, uint32_t b);
// docs/rules.md R9: below 4.5:1 is a warning, below 3:1 is refused.
enum class Contrast { Ok, Warn, Refuse };
Contrast classify_contrast(double ratio);
// The accent is drawn on the card and on the background: the worse of the two decides.
Contrast accent_contrast(uint32_t accent, const Palette &);
// accent == 0 keeps the theme's own; a refused accent is ignored, so the UI can never be made unreadable.
Palette apply_accent(Palette, uint32_t accent);
// The accents offered on the Appearance page. Every one is readable on every theme.
extern const std::array<uint32_t, 6> kAccentChoices;
}  // namespace dh::ui
