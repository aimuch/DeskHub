#include "policy.hpp"
#include <cassert>
#include <cstdio>
int main() {
    dh::DisplayConfig c;assert(dh::valid_config(c));
    auto invalid=c;invalid.dim_s=30;assert(!dh::valid_config(invalid));invalid=c;invalid.off_s=100;assert(!dh::valid_config(invalid));invalid=c;invalid.night_end=1440;assert(!dh::valid_config(invalid));
    assert(dh::in_night(22*60,22*60,7*60));assert(dh::in_night(0,22*60,7*60));assert(!dh::in_night(7*60,22*60,7*60));assert(!dh::in_night(12*60,22*60,7*60));
    dh::DisplayPolicy p;p.update(c,0,true,12*60);assert(p.mode==dh::DisplayMode::Active&&p.effective==50);
    p.update(c,61000,true,12*60);assert(p.mode==dh::DisplayMode::Clock&&p.effective==25);
    p.update(c,121000,true,12*60);assert(p.mode==dh::DisplayMode::Dimmed&&p.effective==8);
    assert(p.touch_down(122000));assert(p.gesture_blocked);p.update(c,122001,true,12*60);assert(p.mode==dh::DisplayMode::Active&&p.effective==50);p.touch_up();assert(!p.gesture_blocked);
    p.update(c,423000,true,12*60);assert(p.mode==dh::DisplayMode::Off);assert(p.touch_down(424000));p.touch_up();
    p.update(c,424001,true,23*60);assert(p.effective==15&&c.brightness==50);p.update(c,424002,true,12*60);assert(p.effective==50);
    p.update(c,424003,false,12*60);assert(p.effective==15);p.fault=true;p.update(c,424004,true,12*60);assert(p.effective==5);p.fault=false;
    // Always-on (off_s == 0): no screen-off, no clock page, no dimming. Night, fault and
    // critical-power limits still apply, and a manual off is still honored.
    c.off_s=0;p.user_off=false;p.last_activity_ms=0;
    for(int64_t idle_ms:{61000,121000,10000000,86400000}){p.update(c,idle_ms,true,12*60);assert(p.mode==dh::DisplayMode::Active&&p.effective==50);}
    p.update(c,86400000,true,23*60);assert(p.mode==dh::DisplayMode::Active&&p.effective==15);
    p.update(c,86400000,false,12*60);assert(p.mode==dh::DisplayMode::Active&&p.effective==15);
    p.fault=true;p.update(c,86400000,true,12*60);assert(p.effective==5);p.fault=false;
    p.critical=true;p.update(c,86400000,true,12*60);assert(p.effective==8);p.critical=false;
    assert(!p.touch_down(86400001));assert(!p.gesture_blocked);p.touch_up();
    p.user_off=true;p.update(c,86400002,true,12*60);assert(p.mode==dh::DisplayMode::Off&&p.effective==0);
    assert(p.touch_down(86400003));p.touch_up();assert(!p.user_off);
    auto dark=c;dark.brightness=0;p.update(dark,86400004,true,12*60);assert(p.mode==dh::DisplayMode::Off);
    c.off_s=300;p.last_activity_ms=86400003;p.update(c,86400003+61000,true,12*60);assert(p.mode==dh::DisplayMode::Clock);
    assert(dh::crc32("123456789",9)==0xcbf43926);
    assert(dh::valid_ntp_server("pool.ntp.org"));assert(dh::valid_ntp_server("192.168.10.1"));assert(dh::valid_ntp_server("ntp.home.arpa"));
    assert(!dh::valid_ntp_server("https://ntp.home.arpa"));assert(!dh::valid_ntp_server("1.2.3.999"));assert(!dh::valid_ntp_server("ntp.local:123"));assert(!dh::valid_ntp_server("-invalid.local"));assert(!dh::valid_ntp_server(""));assert(!dh::valid_ntp_server("x..local"));
    dh::Backoff b;uint32_t old=0;for(int i=0;i<100;++i){auto n=b.next_ms(12345);assert(n>=1000&&n<=30000);assert(n>=old);old=n;}b.reset();assert(b.next_ms(0)==1000);
    puts("policy: config validation, cross-midnight, dim/off/wake consumption, fault priority, preference restoration, monotonic inactivity, CRC, bounded backoff passed");
}
