#!/usr/bin/env python3
"""Mutation check for the icon header and the appearance options: break the code on purpose, the tests must go red.

Usage: scripts/mutation/header_ui.py [name-part ...]
Two parts: the rules of the header model, the stored appearance and the battery fill contrast (ui_model_test), and the wiring of
the icons, the blink, the percent switch and the ring tiers, run against the real UI code in native LVGL (the preview harness,
tests/ui_preview). Works on a copy in a temporary directory. Exit 0 only if both baselines are green and every mutation is caught.
"""
from _engine import firmware_main

MD = "components/ui/ui_model.cpp"
TH = "components/ui/ui_theme.cpp"
THH = "components/ui/include/ui_theme.hpp"
PG = "components/ui/ui_pages.cpp"

MODEL = [
    ("USB is shown without looking at the chip", MD, 'h.usb = p.vbus == "present";', "h.usb = true;"),
    ("the bolt is shown for a battery that is not there", MD, 'b.charging = p.battery == "present" && p.charging == "charging";', 'b.charging = p.charging == "charging";'),
    ("the bolt is shown when full", MD, 'p.charging == "charging";', 'p.charging != "idle";'),
    ("the digits have a percent sign", MD, "b.digits = show_percent ? std::to_string(p.soc_percent) : std::string();", 'b.digits = show_percent ? std::to_string(p.soc_percent) + "%" : std::string();'),
    ("the percent switch is ignored", MD, "b.digits = show_percent ? std::to_string(p.soc_percent) : std::string();", "(void)show_percent; b.digits = std::to_string(p.soc_percent);"),
    ("a fault is shown as unknown", MD, 'b.kind = Kind::Fault;\n        b.digits = "!";', 'b.kind = Kind::Unknown;\n        b.digits = "?";'),
    ("an old sample still shows USB", MD, "    if (!power_fresh(p, now_ms)) {  // nothing about an old sample is shown: no USB, no bolt, no level", "    if (!power_fresh(p, now_ms) && false) {  // nothing about an old sample is shown: no USB, no bolt, no level"),
    ("an absent battery keeps its level", MD, 'if (p.battery == "absent") {\n        b.kind = Kind::None;\n        return h;', 'if (false) {\n        b.kind = Kind::None;\n        return h;'),
    ("connecting is a slash", MD, "kind == WifiKind::Connecting ? LinkIcon::Busy : LinkIcon::Off", "LinkIcon::Off"),
    ("the computer icon follows Wi-Fi", MD, "h.agent = s.agent_ready ? LinkIcon::Ok : LinkIcon::Off;", "h.agent = h.wifi;"),
    ("the blink is the other half", MD, "(now_ms / 500) % 2 == 0;", "(now_ms / 500) % 2 == 1;"),
    ("the blink is twice as slow", MD, "(now_ms / 500) % 2 == 0;", "(now_ms / 1000) % 2 == 0;"),
    ("a clock that runs backwards breaks the blink", MD, "return now_ms < 0 || ", "return "),
    ("the thin ring is thicker", MD, "case RingTier::Thin: return {12, 7};", "case RingTier::Thin: return {14, 7};"),
    ("the thick gauge is too thick", MD, "case RingTier::Thick: return {28, 15};", "case RingTier::Thick: return {28, 18};"),
    ("the standard ring changed", MD, "    return {20, 11};", "    return {20, 12};"),
    ("the old stored text is refused", MD, "if (field.size() != 4 && field.size() != 6 && field.size() != 8 && field.size() != 9) return Appearance{};", "if (field.size() != 6 && field.size() != 8 && field.size() != 9) return Appearance{};"),
    ("a five-field text is accepted", MD, "if (field.size() != 4 && field.size() != 6 && field.size() != 8 && field.size() != 9) return Appearance{};", "if (field.size() < 4 || field.size() > 9 || field.size() == 7) return Appearance{};"),
    ("a bad percent word is accepted", MD, 'else if (field[4] == "nopct") a.show_percent = false;\n        else return Appearance{};', "else a.show_percent = false;"),
    ("a bad ring word is accepted", MD, 'else if (field[5] == "thick") a.ring = RingTier::Thick;\n        else return Appearance{};', "else a.ring = RingTier::Thick;"),
    ("the percent words are swapped in the text", MD, '(a.show_percent ? "pct" : "nopct")', '(a.show_percent ? "nopct" : "pct")'),
    ("the fill is stronger than the digits allow", THH, "kBatteryFillPercent = 40;", "kBatteryFillPercent = 60;"),
    ("blend does not round", TH, "b * static_cast<uint32_t>(100 - percent) + 50) / 100)", "b * static_cast<uint32_t>(100 - percent)) / 100)"),
    ("blend ignores the strength", TH, "percent = std::min(100, std::max(0, percent));", "percent = 50;"),
]

PAGES = [
    ("Wi-Fi never gets a slash", PG, "cut_shown(sh.wifi_cut, h.wifi == LinkIcon::Off);", "cut_shown(sh.wifi_cut, false);"),
    ("the computer never gets a slash", PG, "cut_shown(sh.pc_cut, h.agent != LinkIcon::Ok);", "cut_shown(sh.pc_cut, false);"),
    ("the battery never gets a slash", PG, "cut_shown(sh.bat_cut, h.battery.kind == BatteryView::Kind::None);", "cut_shown(sh.bat_cut, false);"),
    ("connecting does not blink", PG, "h.wifi == LinkIcon::Busy && !blink_on(f.now_ms) ? LV_OPA_20 : LV_OPA_COVER", "LV_OPA_COVER"),
    ("a connected icon blinks", PG, "h.wifi == LinkIcon::Busy && !blink_on(f.now_ms)", "h.wifi != LinkIcon::Off && !blink_on(f.now_ms)"),
    ("USB is always drawn", PG, "set_hidden(sh.usb, !h.usb);", "set_hidden(sh.usb, false);"),
    ("the bolt is always drawn", PG, "set_hidden(sh.bolt, !h.battery.charging);", "set_hidden(sh.bolt, false);"),
    ("the bolt is never drawn", PG, "set_hidden(sh.bolt, !h.battery.charging);", "set_hidden(sh.bolt, true);"),
    ("the fill is never drawn", PG, "        set_hidden(sh.fill, false);\n    } else set_hidden(sh.fill, true);", "        set_hidden(sh.fill, true);\n    } else set_hidden(sh.fill, true);"),
    ("the fill is drawn without a level", PG, "    } else set_hidden(sh.fill, true);\n    set_optional_text(sh.digits", "    } else set_hidden(sh.fill, false);\n    set_optional_text(sh.digits"),
    ("the digits are never updated", PG, "set_optional_text(sh.digits, h.battery.digits);", 'set_optional_text(sh.digits, "");'),
    ("the percent switch is not passed on", PG, "make_header(f.s, f.power, f.now_ms, look.appearance.show_percent)", "make_header(f.s, f.power, f.now_ms, true)"),
    ("the home ring ignores the tier", PG, "g = 4, width = (ring_px(look.appearance.ring).home * d + 108) / 216;", "g = 4, width = 23;"),
    ("the gauge ignores the tier", PG, "gauge(c, 112, ring_px(look.appearance.ring).gauge, P().power)", "gauge(c, 112, 11, P().power)"),
    ("the percent button is the wrong way round", PG, "a.show_percent = data_of(e) == 0;", "a.show_percent = data_of(e) != 0;"),
    ("the ring button always picks standard", PG, "a.ring = static_cast<RingTier>(data_of(e));", "(void)e; a.ring = RingTier::Standard;"),
    ("the digits are centred as a text box, not as ink", PG, "digits_y = 12 - (btr_ui_16.line_height - btr_ui_16.base_line - zero.box_h - zero.ofs_y) - (zero.box_h + 1) / 2;", "digits_y = 1 + (22 - lh(&btr_ui_16)) / 2;"),
    ("the digits are a pixel too low", PG, "- (zero.box_h + 1) / 2;   // 12", "- (zero.box_h + 1) / 2 + 3;   // 12"),
    ("the percent section shows the wrong choice", PG, "a.show_percent ? 0 : 1, on_percent", "a.show_percent ? 1 : 0, on_percent"),
    ("the ring section shows the wrong choice", PG, "static_cast<int>(a.ring), on_ring", "static_cast<int>(RingTier::Standard), on_ring"),
]


def main():
    first = firmware_main(MODEL, "ui_model_test")
    second = firmware_main(PAGES, "deskhub_ui_preview", source="tests/ui_preview", generator="Ninja")
    return 1 if first or second else 0


if __name__ == "__main__":
    raise SystemExit(main())
