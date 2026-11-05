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
    ("unreachable computer defaults to keep", A, "else if(now>=deadline)restore(Reason::Timeout,now);break;", "else if(now>=deadline)save(now);break;"),
    ("save failure reports success", A, "if(!port.save(ssid,password)){", "if(!port.save(ssid,password) && false){"),
]
if __name__ == "__main__":
    raise SystemExit(firmware_main(M, "wifi_change_test"))
