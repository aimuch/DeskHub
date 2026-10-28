#include "ui_theme.hpp"
#include <algorithm>
#include <cmath>

namespace dh::ui {
namespace {
// The tokens of docs/design/README.md. `bg2` is the lower end of the background gradient.
constexpr Palette kMint = {0x03100e, 0x082520, 0x0b1d1a, 0x1b3d37, 0xe6f7f2, 0x8aa8a1, 0x5ee6b8, 0x103a30, 0x0f2622,
                           0x4fd1ff, 0x5be38c, 0xf2b544, 0xc38bff, 0xff7b7b};
constexpr Palette kOcean = {0x03131c, 0x062a3a, 0x0a2130, 0x173c50, 0xe6f6ff, 0x86a9bb, 0x4fc3f7, 0x0f3a52, 0x0f2938,
                            0x4fc3f7, 0x4de0b6, 0xffc857, 0x9fa8ff, 0xff7b7b};
constexpr Palette kAmber = {0x120c08, 0x24150a, 0x1e1510, 0x3a2a1f, 0xf5e9dc, 0xb09a85, 0xff9e4a, 0x3a2412, 0x2a1f17,
                            0x45c8ea, 0x8ddf6c, 0xffb347, 0xdb8ff2, 0xff7b7b};

double channel(uint32_t v) {
    const double c = v / 255.0;
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
double luminance(uint32_t rgb) {
    return 0.2126 * channel((rgb >> 16) & 0xff) + 0.7152 * channel((rgb >> 8) & 0xff) + 0.0722 * channel(rgb & 0xff);
}
// The "selected" tint: the card with 16 % of the accent mixed in.
uint32_t tint(uint32_t card, uint32_t accent) {
    uint32_t out = 0;
    for (int shift : {16, 8, 0}) {
        const uint32_t a = (card >> shift) & 0xff, b = (accent >> shift) & 0xff;
        out |= ((a * 84 + b * 16 + 50) / 100) << shift;
    }
    return out;
}
}  // namespace

const Palette &theme_palette(ThemeId id) {
    switch (id) {
    case ThemeId::Ocean: return kOcean;
    case ThemeId::Amber: return kAmber;
    case ThemeId::Mint: break;
    }
    return kMint;
}
ThemeId theme_from_name(const std::string &name) {
    if (name == "ocean") return ThemeId::Ocean;
    if (name == "amber") return ThemeId::Amber;
    return ThemeId::Mint;
}
const char *theme_name(ThemeId id) {
    switch (id) {
    case ThemeId::Ocean: return "ocean";
    case ThemeId::Amber: return "amber";
    case ThemeId::Mint: break;
    }
    return "mint";
}

double contrast_ratio(uint32_t a, uint32_t b) {
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}
Contrast classify_contrast(double ratio) {
    if (ratio >= 4.5) return Contrast::Ok;
    if (ratio >= 3.0) return Contrast::Warn;
    return Contrast::Refuse;
}
Contrast accent_contrast(uint32_t accent, const Palette &p) {
    const uint32_t selected = tint(p.card, accent);
    // The accent is text and strokes on the background, the card and the selected pill; body text must stay
    // readable on that pill too.
    double worst = std::min({contrast_ratio(accent, p.bg), contrast_ratio(accent, p.bg2), contrast_ratio(accent, p.card),
                             contrast_ratio(accent, selected), contrast_ratio(p.ink, selected)});
    return classify_contrast(worst);
}
Palette apply_accent(Palette p, uint32_t accent) {
    if (accent == 0 || accent_contrast(accent, p) == Contrast::Refuse) return p;
    p.accent = accent;
    p.selected = tint(p.card, accent);
    return p;
}

const std::array<uint32_t, 6> kAccentChoices = {0x5ee6b8, 0x4fc3f7, 0xff9e4a, 0xff7eb6, 0xb69cff, 0xf2e05a};
}  // namespace dh::ui
