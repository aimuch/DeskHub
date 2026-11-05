#!/usr/bin/env python3
"""Break the Wi-Fi session on a temporary copy; prewritten failure cases must catch it."""
from _engine import firmware_main
A = "components/wifi_change/wifi_change_policy.cpp"
M = [
    ("empty SSID accepted", A, "ssid.empty()||ssid.size()>32", "ssid.size()>32"),
    ("short password accepted", A, "password.size()<8||password.size()>63", "password.size()<1||password.size()>63"),
    ("64 byte nonhex PSK accepted", A, "if(password.size()==64){", "if(password.size()==64)return true;\n    if(password.size()==64){"),
    ("ownership ignored", A, "if(!port.acquire()){", "if(port.acquire() && false){"),
    ("same SSID not deduplicated", A, "return v.ssid==n.ssid;", "return v.ssid==n.ssid && false;"),
    ("wrong password never restores", A, "else if(port.join_failure()!=Reason::None)restore(port.join_failure(),now);", "else if(port.join_failure()!=Reason::None)state.reason=port.join_failure();"),
    ("unreachable computer defaults to keep", A, "else if(now>=deadline)restore(Reason::AgentUnreachable,now);break;", "else if(now>=deadline)save(now);break;"),
    ("a failed restore hides why the new one failed", A, "if(!ok&&state.had_previous&&state.reason==Reason::UserReverted)state.reason=Reason::RestoreFailed;", "if(!ok)state.reason=Reason::RestoreFailed;"),
    ("a failed restore after cancel is not said", A, "if(!ok&&state.had_previous&&state.reason==Reason::UserReverted)state.reason=Reason::RestoreFailed;", ""),
    ("no previous Wi-Fi is still restored", A, "    if(!state.had_previous){finish_restore(false,now);return;}   // nothing to go back to: the reason stays what the person is told\n", ""),
    ("going back is not visible", A, "out.restoring=restoring;", ""),
    ("the 60 seconds end as a timeout", A, "else if(now>=deadline)restore(Reason::AgentUnreachable,now);break;", "else if(now>=deadline)restore(Reason::Timeout,now);break;"),
    ("a refused name does not say why", A, "{state.reason=Reason::InvalidInput;return false;}", "return false;"),
    ("a weak signal has four bars", A, "rssi>=-55?4:", "rssi>=-79?4:"),
    ("the middle bars are moved", A, "rssi>=-67?3:rssi>=-78?2:1;", "rssi>=-70?3:rssi>=-74?2:1;"),
    ("a reading that is not a signal has bars", A, "return rssi>=0?0:rssi>=-55?4:", "return rssi>=-55?4:"),
    ("save failure reports success", A, "if(!port.save(ssid,password)){", "if(!port.save(ssid,password) && false){"),
]
if __name__ == "__main__":
    raise SystemExit(firmware_main(M, "wifi_change_test"))
