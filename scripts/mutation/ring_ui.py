#!/usr/bin/env python3
"""Mutation check for the explained ring (docs/design Dev-RingInfo): break the code on purpose, the tests must go red.

Usage: scripts/mutation/ring_ui.py [name-part ...]
Two parts: the rules of the rows and of the line under the ring (ui_model_test), and the wiring of the tap, the sheet, the
dot and the priority of the line, run against the real UI code in native LVGL (the preview harness). Works on a copy in a
temporary directory. Exit 0 only if both baselines are green and every mutation is caught.
"""
from _engine import firmware_main

MD = "components/ui/ui_model.cpp"
PG = "components/ui/ui_pages.cpp"
UI = "components/ui/ui.cpp"

MODEL = [
    ("a row is lit when the ring is dark", MD, "v.row[i] = RingRow{T(l, name), ok ? good_state : bad_state, ok ? T(l, good_note) : bad_note, ok};", "v.row[i] = RingRow{T(l, name), ok ? good_state : bad_state, ok ? T(l, good_note) : bad_note, true};"),
    ("the state of a dark row is the good one", MD, "ok ? good_state : bad_state,", "bad_state.empty() ? good_state : good_state,"),
    ("the note of a dark row is the good one", MD, "ok ? T(l, good_note) : bad_note,", "bad_note.empty() ? T(l, good_note) : T(l, good_note),"),
    ("the first problem is the last", MD, "        if (!v.row[i].ok) { v.first_problem = i; break; }", "        if (!v.row[i].ok) { v.first_problem = i; }"),
    ("the Wi-Fi note is made up", MD, "T(l, Str::RingNotConnected), wifi_text(l, wifi_kind(s)));", "T(l, Str::RingNotConnected), T(l, Str::RingNoteNeedWifi));"),
    ("the computer does not need Wi-Fi first", MD, "const std::string agent_why = !s.wifi_ready ? T(l, Str::RingNoteNeedWifi) :", "const std::string agent_why = false ? T(l, Str::RingNoteNeedWifi) :"),
    ("a pairing that is missing is not said", MD, 'contains(s.agent, "未配对") ? T(l, Str::ValueNotPaired) :', "false ? T(l, Str::ValueNotPaired) :"),
    ("a power fault is read as no reading", MD, "v.power_fault = !r.power && power_fresh(p, now_ms) && p.fault;", "v.power_fault = false;"),
    ("an old fault is a fault", MD, "v.power_fault = !r.power && power_fresh(p, now_ms) && p.fault;", "v.power_fault = !r.power && p.fault;"),
    ("the rows are not in the order of the ring", MD, "set(1, Str::RingComputer, r.agent,", "set(2, Str::RingComputer, r.agent,"),
    ("the line names the computer for the network", MD, "case 0: return T(l, Str::RingProblemWifi);", "case 0: return T(l, Str::RingProblemAgent);"),
    ("the line for the time is the one for power", MD, "case 3: return T(l, Str::RingProblemTime);", "case 3: return T(l, Str::RingProblemPower);"),
    ("the line says nothing when all is well", MD, "    return T(l, Str::RingHint);\n}\n\n// ---- clock ----", '    return std::string();\n}\n\n// ---- clock ----'),
    ("a fault is named as a missing reading", MD, "case 2: return T(l, v.power_fault ? Str::RingNoteFault : Str::RingProblemPower);", "case 2: return T(l, Str::RingProblemPower);"),
]

PAGES = [
    ("a tap on the ring does nothing", PG, "        else request_ring_info();\n    }, data(0), d, d);", "        else (void)0;\n    }, data(0), d, d);"),
    ("a tap on the clock opens the sheet", PG, "request_page(Page::Clock);\n        else request_ring_info();", "request_ring_info();\n        else request_ring_info();"),
    ("the clock area is not where the clock is", PG, "if (at.x >= clock.x1 && at.x <= clock.x2 && at.y >= clock.y1 && at.y <= clock.y2) request_page(Page::Clock);", "if (false) request_page(Page::Clock);"),
    # 0.3.21: no line under the ring; a message takes the place of the date (docs/design v2 OverviewLargeNote)
    ("a message does not hide the date", PG, "    set_hidden(w.date, !n.empty());\n", "    set_hidden(w.date, false);\n"),
    ("a message is never shown", PG, "    set_hidden(w.notice, n.empty());\n", "    set_hidden(w.notice, true);\n"),
    ("the date stays hidden after a message", PG, "    set_hidden(w.date, !n.empty());\n", "    if (!n.empty()) set_hidden(w.date, true);\n"),
    ("the sheet has no close button", PG, "    auto *b = button(box, [](lv_event_t *) {\n        close_ring_sheet();\n        if (closed) closed();\n    }, data(0), 360, 56);", "    auto *b = button(box, [](lv_event_t *) {\n    }, data(0), 360, 56);"),
    ("a tap outside does not close the sheet", PG, "        if (lv_event_get_target(e) != lv_event_get_current_target(e)) return;   // a tap on the sheet itself stays on the sheet\n        close_ring_sheet();", "        (void)e;\n        return;\n        close_ring_sheet();"),
    ("the sheet does not absorb a tap", PG, "    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);   // a tap on the sheet stays on the sheet; only the space around it closes it\n", ""),
    ("the sheet never closes", PG, "    lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);\n    lv_obj_delete_async(d);\n}\nlv_obj_t *build_ring_info", "    (void)d;\n}\nlv_obj_t *build_ring_info"),
    ("the sheet shows the row of the wrong segment", PG, "        const RingRow &r = v.row[i];", "        const RingRow &r = v.row[(i + 1) % 4];"),
    ("a dark row is drawn full", PG, "lv_obj_set_style_bg_opa(dot, r.ok ? LV_OPA_COVER : LV_OPA_TRANSP, 0);", "lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);"),
    ("the sheet opens from an old snapshot", UI, "    Snapshot s = snapshot();\n    overlay_ble(s);\n    build_ring_info(", "    Snapshot s{};\n    overlay_ble(s);\n    build_ring_info("),
]


def main():
    first = firmware_main(MODEL, "ui_model_test")
    second = firmware_main(PAGES, "deskhub_ui_preview", source="tests/ui_preview", generator="Ninja")
    return 1 if first or second else 0


if __name__ == "__main__":
    raise SystemExit(main())
