// Failure cases written before implementation: W1/W2/W3/W4/W5/W10/W11/W12/W14/W16/W17/W18/W19/W21.
// A fake port records durable writes and radio restores. No device or credentials needed.
#include "wifi_change_policy.hpp"
#include <cassert>
#include <iostream>
using namespace dh::wifi_change;
struct Fake : Port {
    bool allowed=true, joined=false, agent=false, paired=true, save_ok=true, restore_ok=true, scan_ok=true, previous=true;
    Reason failure=Reason::None;
    int writes=0, restores=0, releases=0, joins=0;
    bool acquire() override{return allowed;}
    void release() override{++releases;}
    bool scan_start() override{return scan_ok;}
    bool scan_done(std::vector<Network>& out) override{out={{"",Security::Open,-1,false},{"a",Security::Personal,-80,false},{"b",Security::Unsupported,-60,false},{"a",Security::Personal,-20,true}};return true;}
    void scan_stop() override{}
    bool join_start(const std::string&,const std::string&) override{++joins;return true;}
    bool has_ip() override{return joined;}
    Reason join_failure() override{return failure;}
    bool is_paired() override{return paired;}
    bool agent_ready() override{return agent;}
    bool save(const std::string&,const std::string&) override{++writes;return save_ok;}
    bool has_previous() override{return previous;}
    bool restore_start() override{++restores;joined=restore_ok;return true;}
};
void ready(Session& s){assert(s.scan(0));s.tick(1);assert(s.status(1).phase==Phase::Typing);}
int main(){
    assert(!valid_credentials("", "12345678",false));assert(!valid_credentials("a","short",false));
    assert(!valid_credentials("a",std::string(64,'g'),false));assert(valid_credentials("a",std::string(64,'a'),false));
    assert(valid_credentials("a","",true));assert(!valid_credentials("a","",false));assert(!valid_credentials("a","1234567\n",false));
    {Fake f;Session s(f);f.allowed=false;assert(!s.scan(0));assert(f.joins==0&&f.writes==0);}
    {Fake f;Session s(f);ready(s);assert(s.networks().size()==2);assert(s.networks()[0].ssid=="a"&&s.networks()[0].signal==-20);s.touch(100);s.tick(180099);assert(s.status(180099).phase==Phase::Typing);s.tick(180100);assert(s.status(180100).phase==Phase::Idle);assert(f.releases==1);}
    {Fake f;Session s(f);ready(s);assert(!s.begin("a","bad",false,2));assert(s.begin("a","12345678",false,3));assert(f.writes==0);f.failure=Reason::WrongPassword;s.tick(4);s.tick(5);assert(s.status(5).phase==Phase::Reverted);assert(s.status(5).reason==Reason::WrongPassword);assert(f.writes==0&&f.restores==1);}
    {Fake f;Session s(f);ready(s);s.begin("a","12345678",false,2);f.joined=true;s.tick(3);assert(s.status(3).phase==Phase::CheckingAgent);s.tick(15003);assert(s.status(15003).phase==Phase::Asking);assert(f.writes==0);s.tick(75003);s.tick(75004);assert(s.status(75004).phase==Phase::Reverted);assert(f.writes==0);}
    {Fake f;Session s(f);ready(s);s.begin("a","12345678",false,2);f.joined=true;s.tick(3);s.tick(15003);assert(s.keep(15004));assert(f.writes==1);assert(s.status(15004).phase==Phase::Saved);}
    {Fake f;f.paired=false;Session s(f);ready(s);s.begin("open","",true,2);s.tick(3);assert(f.writes==0);f.joined=true;s.tick(4);assert(f.writes==1);}
    {Fake f;f.save_ok=false;Session s(f);ready(s);s.begin("a","12345678",false,2);f.joined=true;s.tick(3);f.agent=true;s.tick(4);s.tick(5);assert(s.status(5).phase==Phase::Reverted);assert(s.status(5).reason==Reason::SaveFailed);}
    {Fake f;f.restore_ok=false;Session s(f);ready(s);s.begin("a","12345678",false,2);s.cancel(3);s.tick(20003);assert(s.status(20003).reason==Reason::RestoreFailed);assert(!s.status(20003).restored);assert(f.releases==1);}
    {Fake f;Session s(f);ready(s);s.begin("a","12345678",false,2);s.tick(20002);s.tick(20003);assert(s.status(20003).phase==Phase::Reverted);assert(f.writes==0);}
    {Fake f;f.scan_ok=false;Session s(f);assert(s.scan(0));assert(s.status(0).reason==Reason::ScanFailed);assert(f.releases==1);}
    // W17: no Wi-Fi before: the wrong password is what the person is told, and nothing is restored.
    {Fake f;f.previous=false;Session s(f);ready(s);s.begin("a","12345678",false,2);f.failure=Reason::WrongPassword;s.tick(3);auto st=s.status(3);
     assert(st.phase==Phase::Reverted&&st.reason==Reason::WrongPassword&&!st.restored&&!st.had_previous&&f.restores==0&&f.releases==1&&!st.restoring);}
    {Fake f;f.previous=false;Session s(f);ready(s);s.begin("a","12345678",false,2);s.cancel(3);auto st=s.status(3);assert(st.reason==Reason::UserReverted&&!st.had_previous&&!st.restored);}
    // W17: a previous Wi-Fi that does not come back does not hide why the new one failed.
    {Fake f;f.restore_ok=false;Session s(f);ready(s);s.begin("a","12345678",false,2);f.failure=Reason::WrongPassword;s.tick(3);f.failure=Reason::None;assert(s.status(3).restoring);s.tick(20003);auto st=s.status(20003);
     assert(st.phase==Phase::Reverted&&st.reason==Reason::WrongPassword&&!st.restored&&st.had_previous&&!st.restoring);}
    // W18: going back is visible; the buttons of the question no longer do anything.
    {Fake f;Session s(f);ready(s);s.begin("a","12345678",false,2);f.joined=true;s.tick(3);s.tick(15003);assert(s.status(15003).phase==Phase::Asking&&!s.status(15003).restoring);
     f.joined=false;f.restore_ok=false;assert(s.revert(15004));auto st=s.status(15004);assert(st.restoring&&st.reason==Reason::UserReverted);assert(!s.keep(15005));assert(f.writes==0);}
    {Fake f;Session s(f);ready(s);s.begin("a","12345678",false,2);f.joined=true;s.tick(3);s.tick(15003);assert(s.revert(15004));s.tick(15005);auto st=s.status(15005);
     assert(st.phase==Phase::Reverted&&st.restored&&!st.restoring&&st.reason==Reason::UserReverted);}
    // W2: the 60 seconds ended: the reason is the computer, not a timeout of the Wi-Fi.
    {Fake f;Session s(f);ready(s);s.begin("a","12345678",false,2);f.joined=true;s.tick(3);s.tick(15003);s.tick(75003);s.tick(75004);auto st=s.status(75004);
     assert(st.phase==Phase::Reverted&&st.reason==Reason::AgentUnreachable&&st.restored&&f.writes==0);}
    // W21: a name typed by hand that is in the list with another kind of security is refused, and says why.
    {Fake f;Session s(f);ready(s);assert(!s.begin("a","",true,2));auto st=s.status(2);assert(st.phase==Phase::Typing&&st.reason==Reason::InvalidInput&&f.joins==0);
     assert(s.begin("a","12345678",false,3));assert(s.status(3).reason==Reason::None);}
    // The bars of the list: strong to weak; a reading that is not a signal has none.
    assert(signal_bars(-40)==4&&signal_bars(-55)==4&&signal_bars(-56)==3&&signal_bars(-67)==3&&signal_bars(-68)==2&&signal_bars(-78)==2&&signal_bars(-79)==1&&signal_bars(-100)==1&&signal_bars(0)==0&&signal_bars(5)==0);
    std::cout<<"wifi_change failure scenarios passed\n";
}
