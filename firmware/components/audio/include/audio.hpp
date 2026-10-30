#pragma once
// The speaker (docs/design Dev-Sound). One task owns the codec; every other task only asks for a tone and never waits for
// the sound. The rules (which tone, when) are in audio_policy.hpp. Not heard by the author: see docs/evidence.
#include "audio_policy.hpp"
#include <string>

namespace dh::audio {
void init();                    // loads the setting and starts the task; the codec is created in the task, not here
bool available();               // the codec answered: false until it has, and false for good when it did not
bool enabled();                 // the setting "sounds on"; off by default
bool set_enabled(bool);         // false when it could not be stored; the setting is then unchanged
// The person pressed "listen": plays a confirm tone, whatever the setting and the night. false: no speaker, or too soon after the last.
bool test();
// A command text of the snapshot, or the end of a pairing: plays when the rules allow it. `quiet_hours`: the night limit is on now.
void on_command(const std::string &text, bool quiet_hours, int64_t now_ms);
void on_pairing_done(int64_t now_ms);
}  // namespace dh::audio
