#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace dh::wifi_change {
enum class Phase { Idle, Scanning, Typing, Testing, CheckingAgent, Asking, Saved, Reverted };
enum class Reason { None, WrongPassword, NotFound, Timeout, AgentUnreachable, UserReverted, HotspotOpen, Busy, InvalidInput, ScanFailed, SaveFailed, RestoreFailed };
enum class Security { Open, Personal, Unsupported };
struct Network { std::string ssid; Security security=Security::Unsupported; int signal=0; bool current=false; };
struct Status { Phase phase=Phase::Idle; Reason reason=Reason::None; int remaining_s=0; int64_t result_ms=0; bool busy=false; bool restored=false; bool restoring=false; bool had_previous=true; };
// The bars of a row of the list: 4 strong to 1 weak, 0 for a reading that is not a signal.
int signal_bars(int rssi);
// Requests never perform radio, TLS or NVS work on the caller's task.
void init();
bool available();
bool hotspot_open();   // why it may not be available: the pairing hotspot owns the radio
bool start_scan();
std::vector<Network> networks();
bool begin(const std::string &ssid,const std::string &password,bool open=false);
bool keep();
bool revert();
void cancel();
void touch();
Status status();
const char *phase_name(Phase);
const char *reason_name(Reason);
}
