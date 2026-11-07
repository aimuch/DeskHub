#!/usr/bin/env python3
"""Mutation check for the v2 device UI (docs/design v2: pages and the home page, the switch page, names and icons of the
input sources, the night theme, no computer metrics): break the code on purpose, the tests must go red.

Usage: scripts/mutation/pages_ui.py [name-part ...]
Two parts: the rules (ui_model_test T24 to T27), and the pages against the real UI code in native LVGL (the preview harness,
cases pages_and_home, wake_goes_home, switch_styles, source_rename, themes, no_metrics, clock_page). Works on a copy in a
temporary directory. Exit 0 only if both baselines are green and every mutation is caught.
"""
import os

from _engine import firmware_main

MD = "components/ui/ui_model.cpp"
TH = "components/ui/ui_theme.cpp"
PG = "components/ui/ui_pages.cpp"
UI = "components/ui/ui.cpp"
RN = "components/ui/ui_rename.cpp"

RULES = [
    # pages
    ("the home page is the first slot", MD, "    for (const auto &s : o) if (s.shown) return s.view;\n", "    return o[0].view;\n"),
    ("hidden pages are in the navigation", MD, "for (const auto &s : o) if (s.shown) out.push_back(s.view);", "for (const auto &s : o) out.push_back(s.view);"),
    ("the last shown page can be hidden", MD, "        if (shown <= 1) return false;", "        if (shown < 1) return false;"),
    ("a move passes the top", MD, "(delta < 0 && slot == 0) || ", ""),
    ("a move of two is taken", MD, "if ((delta != -1 && delta != 1) || slot", "if (delta == 0 || slot"),
    ("a page may be there twice", MD, "if (!view_of(text[2 * i], v) || seen[static_cast<size_t>(v)]) return false;", "if (!view_of(text[2 * i], v)) return false;"),
    ("no page shown is taken", MD, "    if (!any) return false;\n    out = o;", "    out = o;"),
    ("any flag is taken", MD, "        if (flag != '+' && flag != '-') return false;\n", ""),
    ("a refused text is half read", MD, "    PageOrder o{};\n    std::array<bool, kViewCount> seen{};", "    PageOrder &o = out;\n    std::array<bool, kViewCount> seen{};"),
    # appearance
    ("0.3.x keeps the classic theme", MD, "        a.theme = ThemeId::Night;\n    }\n    return a;", "    }\n    return a;"),
    ("0.3.x loses its colour", MD, "        if (a.accent == 0 && a.theme != ThemeId::Ocean) a.accent = theme_palette(a.theme).accent;\n", ""),
    ("a chosen accent is overwritten", MD, "if (a.accent == 0 && a.theme != ThemeId::Ocean)", "if (a.theme != ThemeId::Ocean)"),
    ("night is read from a 0.3.x text", MD, "field[0] == \"night\" && !old", "field[0] == \"night\""),
    ("seven fields are read", MD, "field.size() != 6 && field.size() != 8)", "field.size() != 6 && field.size() != 7 && field.size() != 8)"),
    ("the style is not read", MD, "        else if (field[7] == \"icons\") a.style = SwitchStyle::Icons;", "        else if (field[7] == \"icons\") a.style = SwitchStyle::List;"),
    ("the pages are not stored", MD, "+ \"|\" + serialize_pages(a.pages) + \"|\" +", "+ \"|S+O+C-P-|\" +"),
    # input sources
    ("a port is found inside a word", MD, "        if (w[i] == \"hdmi\") return SourceIcon::Hdmi;", "        if (w[i].find(\"hdmi\") != std::string::npos) return SourceIcon::Hdmi;"),
    ("dp is found inside a word", MD, "if (w[i] == \"dp\" || w[i] == \"displayport\")", "if (w[i].find(\"dp\") != std::string::npos || w[i] == \"displayport\")"),
    ("the number stays on the word", MD, "        while (!w.empty() && w.back() >= '0' && w.back() <= '9') w.pop_back();\n", ""),
    ("usb alone is USB-C", MD, "if ((w[i] == \"usb\" || w[i] == \"type\") && i + 1 < w.size() && w[i + 1] == \"c\")", "if (w[i] == \"usb\" || w[i] == \"type\")"),
    ("the local name is not used", MD, "            if (!local.name.empty()) { v.name = local.name; v.renamed = true; }\n", ""),
    ("the local icon is not used", MD, "            if (local.icon != SourceIcon::Auto) v.icon = local.icon;\n", ""),
    ("an offline source is enabled", MD, "if (local.icon != SourceIcon::Auto) v.icon = local.icon;\n        }\n        v.enabled = s.agent_ready;", "if (local.icon != SourceIcon::Auto) v.icon = local.icon;\n        }\n        v.enabled = true;"),
    ("a 13th source is taken", MD, "    if (names.size() >= kMaxSourceNames) return nullptr;\n", ""),
    ("a long name is taken", MD, "!name.empty() && name.size() <= kMaxSourceName && valid_utf8_text(name)", "!name.empty() && valid_utf8_text(name)"),
    ("a tab is taken", MD, "if (c < 0x20 || c == 0x7f) return false;", "if (c == 0x7f) return false;"),
    ("an empty entry is kept", MD, "    e->name = name;\n    drop_empty(names);\n", "    e->name = name;\n"),
    ("a stored id may be there twice", MD, "if (find_source(out, n.id) || out.size() >= kMaxSourceNames) return {};", "if (out.size() >= kMaxSourceNames) return {};"),
    ("an unknown icon is read", MD, "    return false;\n}\n// The words of an input id", "    return true;\n}\n// The words of an input id"),
    # the night theme
    ("night is not black", TH, "constexpr Palette kNight = {0x000000, 0x000000, 0x070b10", "constexpr Palette kNight = {0x03100e, 0x000000, 0x070b10"),
    ("an unknown theme is mint", TH, "    if (name == \"mint\") return ThemeId::Mint;\n    return ThemeId::Night;", "    return ThemeId::Mint;"),
]

PAGES = [
    ("the device starts on the overview", UI, "    page = desired = home_page();   // the device starts on the home page\n", ""),
    ("waking stays on the clock", UI, "        if (desired == Page::Clock) desired = home_page();\n", ""),
    # Not listed: "waking overrides the button" (desired = home_page() without the guard) is equivalent today. The press of a
    # touch wakes the screen in its own tick, before the click that picks a page, so the guard cannot be seen from outside.
    ("hiding the last page is not explained", UI, "        appearance_error = tr(look.lang, Str::PagesKeepOne);\n", "        appearance_error.clear();\n"),
    ("every switch is the first row", PG, "void on_pages_toggle(lv_event_t *e) { request_pages_toggle(data_of(e)); }", "void on_pages_toggle(lv_event_t *) { request_pages_toggle(0); }"),
    ("up moves down", PG, "void on_pages_up(lv_event_t *e) { request_pages_move(data_of(e), -1); }", "void on_pages_up(lv_event_t *e) { request_pages_move(data_of(e), +1); }"),
    ("the navigation goes to the first page", PG, "sh.nav_page[k] = settings ? Page::Settings : page_of(views[k]);", "sh.nav_page[k] = settings ? Page::Settings : page_of(views[0]);"),
    ("the navigation ignores the order", PG, "    const auto views = nav_views(look.appearance.pages);\n", "    const auto views = nav_views(default_pages());\n"),
    ("the style is ignored", PG, "    if (look.appearance.style == SwitchStyle::List) {", "    if (true) {"),
    ("the style is not stored", UI, "    next.style = style;\n", "    next.style = SwitchStyle::List; (void)style;\n"),
    ("every tile runs the first source", PG, "auto *b = button(grid, on_action, data(static_cast<int>(i)), tw, th);", "auto *b = button(grid, on_action, data(0), tw, th);"),
    ("the name is not stored", UI, "    const bool ok = set_source_name(next, edit_id, name);", "    const bool ok = true; (void)name;"),
    ("done stores nothing", RN, "    request_source_name(editor.value());   // a failure is said on the edit page\n", ""),
    ("back stores the name", RN, "void rename_back() { editor.clear(); closing = true;", "void rename_back() { request_source_name(editor.value()); editor.clear(); closing = true;"),
    ("every icon is the TV", PG, "request_source_icon(static_cast<SourceIcon>(data_of(e)));", "request_source_icon(SourceIcon::Tv); (void)e;"),
    ("a failed save is not said", UI, "if (!valid || !sources_save(serialize_sources(next))) appearance_error = tr(look.lang, Str::SaveFailed);", "if (!valid || !sources_save(serialize_sources(next))) appearance_error.clear();"),
    ("a failed save is shown as done", UI, "    if (!valid || !sources_save(serialize_sources(next))) appearance_error", "    sources = next;\n    if (!valid || !sources_save(serialize_sources(next))) appearance_error"),
    ("restore keeps the local name", UI, "    reset_source(next, edit_id);\n", ""),
    ("a factory reset keeps the names on screen", UI, "appearance = Appearance{}; sources.clear();", "appearance = Appearance{};"),
    ("the Agent is asked for metrics", UI, "    if (s.metrics_visible) mutate([](Snapshot &v, void *) { v.metrics_visible = false; });", "    if (!s.metrics_visible) mutate([](Snapshot &v, void *) { v.metrics_visible = true; });"),
    ("night cannot be chosen", PG, "const ThemeId ids[4] = {ThemeId::Night, ThemeId::Mint,", "const ThemeId ids[4] = {ThemeId::Mint, ThemeId::Mint,"),
    ("the clock dots are always lit", PG, "for (size_t i = 0; i < 4; ++i) set_bg(w.clock_dot[i], lit[i] ? colors[i] : P().edge);", "for (size_t i = 0; i < 4; ++i) set_bg(w.clock_dot[i], colors[i]); (void)lit;"),
    ("the clock dots are in the wrong order", PG, "const bool lit[4] = {r.wifi, r.agent, r.power, r.time};\n    const uint32_t colors[4] = {P().wifi, P().agent, P().power, P().clock};\n    for (size_t i = 0; i < 4; ++i) set_bg(w.clock_dot",
     "const bool lit[4] = {r.agent, r.wifi, r.power, r.time};\n    const uint32_t colors[4] = {P().wifi, P().agent, P().power, P().clock};\n    for (size_t i = 0; i < 4; ++i) set_bg(w.clock_dot"),
]


def main():
    first = firmware_main(RULES, "ui_model_test")
    os.environ["DESKHUB_PREVIEW_CASE"] = "pages_and_home,wake_goes_home,switch_styles,source_rename,themes,no_metrics,clock_page"
    second = firmware_main(PAGES, "deskhub_ui_preview", source="tests/ui_preview", generator="Ninja")
    return 1 if first or second else 0


if __name__ == "__main__":
    raise SystemExit(main())
