#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace dh::touch_detail {
struct Point { uint16_t x=0, y=0, strength=0; uint8_t track_id=0; };
struct Output { bool pressed=false, cancel=false; Point point{}; };

// This class has no hardware or allocation. Its caller supplies a short lock.
// Edges remain ordered; only consecutive motion samples may be coalesced.
class SampleQueue {
public:
    static constexpr size_t capacity=16;
    static constexpr uint32_t stale_ms=1000;
    void publish(bool pressed,Point point,uint32_t observed_ms) {
        last_valid_ms_=observed_ms;has_valid_=true;hard_error_=false;
        ++samples;
        if(inhibit_){
            source_pressed_=pressed;
            if(!pressed)inhibit_=false;
            return;
        }
        const bool edge=pressed!=source_pressed_;
        source_pressed_=pressed;
        if(!edge&&!pressed)return;
        if(!edge&&count_){
            auto &last=events_[(head_+count_-1)%capacity];
            if(last.pressed&&!last.edge){last={true,false,point,observed_ms};return;}
        }
        if(!edge&&!count_&&delivered_pressed_&&same(point,delivered_point_))return;
        if(count_==capacity){++overflows;cancel();return;}
        events_[(head_+count_)%capacity]={pressed,edge,point,observed_ms};++count_;
    }
    void failure(){hard_error_=true;cancel();}
    Output consume(uint32_t now) {
        const bool old_sample=has_valid_&&static_cast<uint32_t>(now-last_valid_ms_)>stale_ms;
        const bool old_event=count_&&static_cast<uint32_t>(now-events_[head_].observed_ms)>stale_ms;
        if((old_sample||old_event)&&!stale_latched_){++stale_cancellations;cancel();stale_latched_=true;}
        if(!old_sample&&!old_event)stale_latched_=false;
        if(cancel_pending_){cancel_pending_=false;delivered_pressed_=false;return {false,true,{}};}
        if(count_){
            const auto event=events_[head_];head_=(head_+1)%capacity;--count_;
            delivered_pressed_=event.pressed;delivered_point_=event.point;
        }
        return {delivered_pressed_,false,delivered_point_};
    }
    bool fresh(uint32_t now) const {return has_valid_&&!hard_error_&&static_cast<uint32_t>(now-last_valid_ms_)<=stale_ms;}
    bool recovering() const {return inhibit_;}
    bool has_valid() const {return has_valid_;}
    int64_t age(uint32_t now) const {return has_valid_?static_cast<uint32_t>(now-last_valid_ms_):-1;}
    size_t queued() const{return count_;}
    uint32_t samples=0,overflows=0,stale_cancellations=0;
private:
    struct Event {bool pressed,edge;Point point;uint32_t observed_ms;};
    static bool same(Point a,Point b){return a.x==b.x&&a.y==b.y&&a.track_id==b.track_id;}
    void cancel(){head_=count_=0;cancel_pending_=true;inhibit_=true;source_pressed_=false;}
    std::array<Event,capacity> events_{};
    size_t head_=0,count_=0;
    bool source_pressed_=false,delivered_pressed_=false,inhibit_=true;
    bool has_valid_=false,hard_error_=false,cancel_pending_=true,stale_latched_=false;
    uint32_t last_valid_ms_=0;
    Point delivered_point_{};
};
}
