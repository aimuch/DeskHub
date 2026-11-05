#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace dh::wifi_change {
enum class Phase { Idle, Scanning, Typing, Testing, CheckingAgent, Asking, Saved, Reverted };
enum class Reason { None, WrongPassword, NotFound, Timeout, AgentUnreachable, UserReverted, HotspotOpen, Busy, InvalidInput, ScanFailed, SaveFailed, RestoreFailed };
enum class Security { Open, Personal, Unsupported };
struct Network { std::string ssid; Security security=Security::Unsupported; int signal=0; bool current=false; };
struct Status { Phase phase=Phase::Idle; Reason reason=Reason::None; int remaining_s=0; int64_t result_ms=0; bool busy=false; bool restored=false; };
// Requests never perform radio, TLS or NVS work on the caller's task.
void init();
bool available();
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
