#!/usr/bin/env python3
"""Mutation check for the Usage page (docs/desktop-app-design.md 13.12): break the code on purpose, the tests must go red.

Usage: scripts/mutation/usage_page.py [name-part ...]
Three parts: the message (ble_protocol_test B7), the page order and the words (ui_model_test T28, T29), and the page against
the real UI code (the preview harness, case usage_page). Works on a copy in a temporary directory. Exit 0 only if every
baseline is green and every mutation is caught.
"""
import os

from _engine import firmware_main

P = "components/ble_link/ble_protocol.cpp"
M = "components/ui/ui_model.cpp"
G = "components/ui/ui_pages.cpp"
U = "components/ui/ui.cpp"

PROTOCOL = [
    ("ok:false may carry more", P, '            if (!keys(j, {"op", "ok"})) return bad("usage fields");\n', ""),
    ("a field more or less is taken", P, '        if (!keys(j, {"op", "ok", "p", "plan", "w", "c", "e", "t", "f", "lm", "ls", "lms", "la"})) return bad("usage fields");\n', ""),
    ("upper case names", P, "if (!((*c >= 'a' && *c <= 'z') ||", "if (!((*c >= 'A' && *c <= 'z') ||"),
    ("long names", P, "if (!s || strlen(s) > 16) return false;", "if (!s || strlen(s) > 64) return false;"),
    ("three windows", P, "if (!cJSON_IsArray(w) || cJSON_GetArraySize(w) > 2) return bad(\"usage windows\");", "if (!cJSON_IsArray(w) || cJSON_GetArraySize(w) > 3) return bad(\"usage windows\");"),
    ("a window with more fields", P, '!keys(item, {"m", "r", "s"}) ||', ""),
    ("more than 100 percent", P, '"r"), 0, 100, &rem)', '"r"), 0, 1000, &rem)'),
    ("more errors than calls", P, '"e"), 0, calls, &errors)', '"e"), 0, 1e9, &errors)'),
    ("a long model", P, "!good_text(model, 32)", "!good_text(model, 100)"),
]
MODEL = [
    ("Usage is not the fifth page", M, "{View::Power, false}, {View::Usage, false}}}; }", "{View::Usage, false}, {View::Power, false}}}; }"),
    ("old texts are refused", M, '? given + "U-" : given;', "? given : given;"),
    ("a fifth page may be shown", M, "    if (!o[slot].shown && shown >= kMaxShownPages) return false;", "    if (!o[slot].shown && shown > kMaxShownPages) return false;"),
    ("five shown are read", M, "if (!any || shown > kMaxShownPages) return false;", "if (!any || shown > 99) return false;"),
    ("stale at once", M, "    if (age_ms > ble::kUsageStaleMs) {   // G3", "    if (age_ms > 1000) {   // G3"),
    ("never stale", M, "    if (age_ms > ble::kUsageStaleMs) {   // G3", "    if (age_ms < 0) {   // G3"),
    ("old numbers when Magpie is off", M, "    if (!u->ok) { v.state = UsageView::State::Offline; v.status = T(l, Str::UsageOffline); return v; }   // G2: no old numbers",
     "    if (!u->ok) { v.state = UsageView::State::Offline; v.status = T(l, Str::UsageOffline); }"),
    ("a weekday without the time", M, "    if (!time_valid || now < kClockEpoch) return", "    if (false && (!time_valid || now < kClockEpoch)) return"),
    ("the age at the App is not added", M, "    const int64_t left = static_cast<int64_t>(w.reset_s) - age_s;", "    const int64_t left = static_cast<int64_t>(w.reset_s) - age_s * 0;"),
    ("100% with no calls", M, "    if (u->calls > 0) {   // G6", "    if (true) {   // G6"),
    ("tokens in thousands only", M, "    while (v >= 999.95 && u < 3) { v /= 1000.0; ++u; }", "    v /= 1000.0; ++u;"),
    ("a failed request looks fine", M, "    v.failed = u->latest_status != 0 && (u->latest_status < 200 || u->latest_status >= 400);", "    v.failed = false;"),
]
PAGES = [
    ("the fifth row is said to be the last shown", U, "appearance_error = tr(look.lang, showing ? Str::PagesAtMost : Str::PagesKeepOne);", "appearance_error = tr(look.lang, showing && false ? Str::PagesAtMost : Str::PagesKeepOne);"),
    ("the page shows the old numbers when Magpie is off", G, "    set_hidden(uw.stats, !data);\n", "    set_hidden(uw.stats, false);\n"),
    # not here: "the waiting message stays over the numbers" is equivalent: with numbers its text is empty
    ("the second window is always drawn", G, "        const bool shown = data && i < v.quotas.size();", "        const bool shown = data;"),
    ("the model is cut with dots", G, "    uw.model = label_at(uw.latest, \"\", 14, 10 + lh(&btr_ui_16) + 8, kContentW - 28, &btr_ui_20, P().ink);",
     "    uw.model = label_at(uw.latest, \"\", 14, 10 + lh(&btr_ui_16) + 8, kContentW - 228, &btr_ui_20, P().ink);"),
]

if __name__ == "__main__":
    a = firmware_main(PROTOCOL, "ble_protocol_test")
    b = firmware_main(MODEL, "ui_model_test")
    os.environ["DESKHUB_PREVIEW_CASE"] = "usage_page"
    c = firmware_main(PAGES, "deskhub_ui_preview", source="tests/ui_preview", generator="Ninja")
    raise SystemExit(1 if a or b or c else 0)
