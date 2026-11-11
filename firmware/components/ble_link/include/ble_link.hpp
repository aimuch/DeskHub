#pragma once
// The Bluetooth link to the DeskHub App on the Mac (docs/desktop-app-design.md 13; rules K1 and K2). NimBLE peripheral:
// one service, every characteristic needs an encrypted, authenticated link (LE Secure Connections, numeric comparison).
// A new computer is accepted only while the pairing window is open and after the person confirmed the digits on the
// screen. Without Bluetooth in the configuration every call is a harmless no-op.
#include "ble_protocol.hpp"
#include "ble_update.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace dh::ble {
struct Status {
    bool running = false;          // the stack is up and advertising or connected
    bool connected = false;
    bool secure = false;           // encrypted, authenticated and bonded: only then do sources and requests count
    int bonds = 0;                 // computers that may connect (at most 2)
    bool pairing_open = false;
    int64_t pairing_left_ms = 0;
    bool confirm_pending = false;  // the six digits wait for the person
    uint32_t digits = 0;
    int64_t confirm_left_ms = 0;
    bool waiting = false;          // a switch request waits for the App's result
};

void start();                                  // before the display and Wi-Fi (H2): it needs internal memory first
Status status();
std::vector<Source> sources();                 // what the App sent; empty unless the link is secure
std::string current();                         // the input the App read back from the monitor; "" when not known
bool usage(Usage &out, int64_t &age_ms);       // the last usage the App sent (13.12) and how long ago; false: none yet
bool request(const std::string &id);           // false: no secure link, one is waiting, or the id is not in the list
// The to-do board (13.13). The list is kept when the link drops (it goes stale); todos_off hides it until a new list comes.
struct TodoOutcome {
    bool timeout = false;          // no answer in time, or the link dropped: not known whether it changed (L11)
    TodoAction action = TodoAction::Done;
    bool ok = false;
    std::string title, detail;
};
struct TodoState {
    bool has_list = false;
    TodoList list;
    std::vector<TodoListName> lists;   // the lists a to-do can move into (13.14)
    int64_t age_ms = 0;            // since the last complete list
    std::string off_why;           // "access" or "setting" after todos_off; "" otherwise
    bool waiting = false;          // an action waits for the App
    bool has_outcome = false;
    TodoOutcome outcome;           // the last action's end
    int64_t outcome_age_ms = 0;
};
TodoState todos();
bool todo_act(const std::string &id, TodoAction);   // false: no secure link, one is waiting, or the id is not shown
bool todo_edit(const TodoEdit &);                   // the same rules, and the edit must be valid (13.14 M1)
void open_pairing();
void close_pairing();
void answer_digits(bool same);                 // "the same, pair" or "not the same"
bool forget_all();                             // every bond; the link drops. true when nothing is stored afterwards

// The firmware update over Bluetooth (docs/desktop-app-design.md 13.11) asks management, through these (main wires them:
// ble_link cannot depend on management, which depends on the UI, which depends on ble_link). Unset: every update is refused.
struct UpdateHooks {
    bool (*allowed)(std::string &why) = nullptr;                                  // no other update, no self-test, power steady
    update::Checked (*verify)(const std::string &envelope, void *ctx) = nullptr;  // signature with update_key + the 8 fields
    void (*active)(bool on) = nullptr;                                            // a Bluetooth update runs: the Agent's waits
    bool (*boot_pending)() = nullptr;                                             // self-test not passed yet; cheap (NimBLE task)
};
void set_update_hooks(const UpdateHooks &);
// The fingerprint of the update key, for the info characteristic ("" when none). Computed by the caller, never in the
// NimBLE task: parsing the key there needs more than its 4 KB of stack, and gave "" (2026-11-07, on the device).
void set_update_key(const std::string &fingerprint);
}  // namespace dh::ble
