#!/usr/bin/env python3
"""Mutation check for the pages around the Wi-Fi change (W17 to W22, the list, the Connect page, the Device page).

Usage: scripts/mutation/wifi_pages_ui.py [name-part ...]
Breaks the real UI code on purpose and runs it in native LVGL (the preview harness, case wifi_pages); every mutation must turn a
check red. Works on a copy in a temporary directory. Exit 0 only if the baseline is green and every mutation is caught.
"""
import os

from _engine import firmware_main

W = "components/ui/ui_wifi.cpp"
P = "components/ui/ui_pages.cpp"
U = "components/ui/ui.cpp"
K = "components/ui/include/ui_keyboard.hpp"
M = "components/ui/ui_model.cpp"

PAGES = [
    # the result page
    ("cancel closes the page", W, "    case 2: case 4: wc::revert(); break;", "    case 2: wc::revert(); break;"),
    ("buttons stay while it goes back", W, "    if(s.restoring) return;   // going back takes a moment", "    // going back takes a moment"),
    ("the title hides that it goes back", W, "s.restoring ? (s.reason==wc::Reason::UserReverted ? txt(\"正在改回\",\"Going back\") : failure(s.reason)) : asking", "asking"),
    ("the note hides that it goes back", W, "s.restoring ? txt(\"没有保存。正在连回原来的 Wi-Fi\",\"Not saved. Going back to the previous Wi-Fi\") : asking", "asking"),
    ("no previous Wi-Fi is said to be restored", W, "s.reason==wc::Reason::UserReverted && !s.had_previous ? txt(\"已取消\",\"Cancelled\") : failure(s.reason)", "failure(s.reason)"),
    ("no previous Wi-Fi is said to be retried", W, "s.had_previous ? txt(\"没有保存。原来的 Wi-Fi 也没连上，设备会继续重试。\",\"Not saved. Previous Wi-Fi unavailable. Retrying.\") : txt(\"没有保存。\",\"Not saved.\")", "txt(\"没有保存。原来的 Wi-Fi 也没连上，设备会继续重试。\",\"Not saved. Previous Wi-Fi unavailable. Retrying.\")"),
    ("the computer is called a timeout", W, "    case wc::Reason::AgentUnreachable:return txt(\"连不上电脑\",\"Computer unreachable\");\n", ""),
    ("a change of going back does not redraw", W, " || s.restoring!=last_restoring) dirty=true;", ") dirty=true;"),
    # the list and the keyboard
    ("the list shows numbers", W, "n.security==wc::Security::Open ? txt(\"开放\",\"Open\") : \"\";", "n.security==wc::Security::Open ? txt(\"开放\",\"Open\") : std::to_string(n.signal)+\" dBm\";"),
    ("the list does not say 2.4 GHz", W, "    label(list,txt(\"只显示 2.4 GHz 的 Wi-Fi\",\"Only 2.4 GHz networks are shown\"),400,&btr_ui_16,P().muted);\n", ""),
    ("a refused choice is not said", W, "    if(refused) label(list,", "    if(false) label(list,"),
    ("a refused choice stays on the result page", W, "{ refused=true; view=View::List; dirty=true; }", "{ refused=true; dirty=true; }"),
    ("a refused Done is silent", W, "    else { set_text(hint,txt(\"现在不能连接，请稍等\",\"Not ready to connect. Wait a moment.\")); set_ink(hint,P().power); }\n", ""),
    ("the line does not say how many are missing", W, "const bool short_now=editor.size()>0 && editor.missing()>0;", "const bool short_now=false && editor.size()>0;"),
    ("the count always says 64", K, "size_t limit() const { return password_ ? (length_ == 64 ? 64 : 63) : 32; }", "size_t limit() const { return password_ ? 64 : 32; }"),
    ("a hex key is not named", W, "(editor.size()==64 ? txt(\"64 位十六进制密码\",\"64 hex digit key\") : txt(\"最多 63 个字符\",\"63 characters at most\"))", "txt(\"最多 63 个字符\",\"63 characters at most\")"),
    # the Connect page
    ("the name is lost on refresh", P, "const auto rows = make_connect(look.lang, f.s, f.paired, f.wifi_name);\n    for (size_t i", "const auto rows = make_connect(look.lang, f.s, f.paired);\n    for (size_t i"),
    ("the device ID stays on the Connect page", P, "        if (i == 5) { w.connect_values.push_back(nullptr); continue; }\n", ""),
    ("every reason is the hotspot", P, "wifi_change::hotspot_open() ? (look.lang", "true ? (look.lang"),
    ("a tap on pair does nothing", P, "[](lv_event_t *) { request_pairing(); }", "[](lv_event_t *) {}"),
    ("the paired device says start", P, "T(f.paired ? Str::PairAgain : Str::PairStart)", "T(Str::PairStart)"),
    ("the unpaired device says again", P, "T(f.paired ? Str::PairAgain : Str::PairStart)", "T(Str::PairAgain)"),
    ("no backend crashes the tap", U, "    if (pairing_request) pairing_request();\n", "    pairing_request();\n"),
    # the Device page
    ("the page shows ok", P, "if (!f.power.reason.empty() && f.power.reason != \"ok\") detail +=", "detail +="),
    ("the page shows an idle update", P, "if (!f.s.update_state.empty() && f.s.update_state != \"idle\") {", "{"),
]

if __name__ == "__main__":
    os.environ["DESKHUB_PREVIEW_CASE"] = "wifi_pages"
    raise SystemExit(firmware_main(PAGES, "deskhub_ui_preview", source="tests/ui_preview", generator="Ninja"))
