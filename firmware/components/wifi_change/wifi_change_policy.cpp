#include "wifi_change_policy.hpp"
#include <algorithm>
namespace dh::wifi_change {
namespace {
void clear_secret(std::string &s){volatile char *p=s.empty()?nullptr:&s[0];for(size_t i=0;i<s.size();++i)p[i]=0;s.clear();}
}
bool valid_credentials(const std::string &ssid,const std::string &password,bool open){
    if(ssid.empty()||ssid.size()>32||ssid.find('\0')!=std::string::npos)return false;
    if(open)return password.empty();
    if(password.size()==64){for(unsigned char c:password)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))return false;return true;}
    if(password.size()<8||password.size()>63)return false;
    for(unsigned char c:password)if(c<32||c>126)return false;
    return true;
}
Session::~Session(){wipe();}
void Session::wipe(){clear_secret(password);clear_secret(ssid);}
void Session::release(){wipe();if(held){port.release();held=false;}}
bool Session::scan(int64_t now){
    if(restoring||(state.busy&&state.phase!=Phase::Typing))return false;
    if(!held){if(!port.acquire()){state.reason=Reason::Busy;return false;}held=true;}
    state.phase=Phase::Scanning;state.reason=Reason::None;state.busy=true;state.restored=false;deadline=now+10000;list.clear();
    if(!port.scan_start()){state.phase=Phase::Idle;state.reason=Reason::ScanFailed;state.busy=false;state.result_ms=now;release();}
    return true;
}
bool Session::touch(int64_t now){if(state.phase!=Phase::Typing||!held)return false;deadline=now+180000;return true;}
bool Session::begin(const std::string &s,const std::string &p,bool open,int64_t now){
    if(state.phase!=Phase::Typing||!held||!valid_credentials(s,p,open))return false;
    // A scanned unsupported network cannot bypass the disabled UI entry.
    for(const auto &n:list)if(n.ssid==s&&(n.security==Security::Unsupported||((n.security==Security::Open)!=open)))return false;
    ssid=s;password=p;state.phase=Phase::Testing;state.reason=Reason::None;state.busy=true;state.restored=false;deadline=now+20000;
    if(!port.join_start(ssid,password))restore(Reason::NotFound,now);
    return true;
}
void Session::save(int64_t now){
    if(!port.has_ip()){restore(Reason::Timeout,now);return;}
    if(!port.save(ssid,password)){restore(Reason::SaveFailed,now);return;}
    state.phase=Phase::Saved;state.reason=Reason::None;state.result_ms=now;state.busy=false;deadline=0;release();
}
bool Session::keep(int64_t now){if(state.phase!=Phase::Asking||restoring||now>=deadline)return false;save(now);return true;}
void Session::restore(Reason reason,int64_t now){
    if(restoring)return;
    wipe();state.reason=reason;state.busy=true;restoring=true;deadline=now+20000;
    if(!port.restore_start())finish_restore(false,now);
}
void Session::finish_restore(bool ok,int64_t now){state.phase=Phase::Reverted;state.restored=ok;if(!ok)state.reason=Reason::RestoreFailed;state.busy=false;state.result_ms=now;restoring=false;deadline=0;release();}
bool Session::revert(int64_t now){if(!held||restoring)return false;cancel(now);return true;}
void Session::cancel(int64_t now){
    if(!held||restoring)return;
    if(state.phase==Phase::Scanning)port.scan_stop();
    if(state.phase==Phase::Scanning||state.phase==Phase::Typing){state.phase=Phase::Idle;state.reason=Reason::UserReverted;state.busy=false;deadline=0;release();}
    else restore(Reason::UserReverted,now);
}
void Session::tick(int64_t now){
    if(restoring){if(port.has_ip())finish_restore(true,now);else if(now>=deadline)finish_restore(false,now);return;}
    switch(state.phase){
    case Phase::Scanning:{
        std::vector<Network> found;
        if(port.scan_done(found)){
            std::sort(found.begin(),found.end(),[](const Network&a,const Network&b){return a.signal>b.signal;});
            for(auto &n:found)if(!n.ssid.empty()&&std::none_of(list.begin(),list.end(),[&](const Network &v){return v.ssid==n.ssid;}))list.push_back(std::move(n));
            state.phase=Phase::Typing;deadline=now+180000;
        }else if(now>=deadline){port.scan_stop();state.phase=Phase::Idle;state.reason=Reason::ScanFailed;state.busy=false;state.result_ms=now;release();}
        break;}
    case Phase::Typing:if(now>=deadline){state.phase=Phase::Idle;state.reason=Reason::Timeout;state.busy=false;release();}break;
    case Phase::Testing:
        if(port.has_ip()){if(!port.is_paired())save(now);else{state.phase=Phase::CheckingAgent;deadline=now+15000;}}
        else if(port.join_failure()!=Reason::None)restore(port.join_failure(),now);
        else if(now>=deadline)restore(Reason::Timeout,now);
        break;
    case Phase::CheckingAgent:
        if(!port.has_ip())restore(Reason::Timeout,now);
        else if(port.agent_ready())save(now);
        else if(now>=deadline){state.phase=Phase::Asking;state.reason=Reason::AgentUnreachable;deadline=now+60000;}
        break;
    case Phase::Asking:if(!port.has_ip())restore(Reason::Timeout,now);else if(now>=deadline)restore(Reason::Timeout,now);break;
    default:break;
    }
}
Status Session::status(int64_t now)const{auto out=state;out.remaining_s=deadline>now?static_cast<int>((deadline-now+999)/1000):0;return out;}
const char *phase_name(Phase p){static const char* const names[]={"idle","scanning","typing","testing","checking_agent","asking","saved","reverted"};auto i=static_cast<unsigned>(p);return i<8?names[i]:"idle";}
const char *reason_name(Reason r){static const char* const names[]={"NONE","WRONG_PASSWORD","NOT_FOUND","TIMEOUT","AGENT_UNREACHABLE","USER_REVERTED","HOTSPOT_OPEN","BUSY","INVALID_INPUT","SCAN_FAILED","SAVE_FAILED","RESTORE_FAILED"};auto i=static_cast<unsigned>(r);return i<12?names[i]:"BUSY";}
}
