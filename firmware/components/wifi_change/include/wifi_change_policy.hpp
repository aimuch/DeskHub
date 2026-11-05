#pragma once
#include "wifi_change.hpp"
namespace dh::wifi_change {
bool valid_credentials(const std::string &,const std::string &,bool open);
struct Port {
    virtual ~Port()=default;
    virtual bool acquire()=0;
    virtual void release()=0;
    virtual bool scan_start()=0;
    virtual bool scan_done(std::vector<Network>&)=0;
    virtual void scan_stop()=0;
    virtual bool join_start(const std::string&,const std::string&)=0;
    virtual bool has_ip()=0;
    virtual Reason join_failure()=0;
    virtual bool is_paired()=0;
    virtual bool agent_ready()=0;
    virtual bool save(const std::string&,const std::string&)=0;
    virtual bool has_previous()=0;
    virtual bool restore_start()=0;
};
class Session {
public:
    explicit Session(Port &p):port(p){}
    ~Session();
    bool scan(int64_t now);
    bool begin(const std::string&,const std::string&,bool open,int64_t now);
    bool keep(int64_t now);
    bool revert(int64_t now);
    void cancel(int64_t now);
    bool touch(int64_t now);
    void tick(int64_t now);
    Status status(int64_t now) const;
    const std::vector<Network>& networks() const{return list;}
private:
    Port &port;
    Status state;
    std::vector<Network> list;
    std::string ssid,password;
    bool held=false,restoring=false;
    int64_t deadline=0;
    void wipe();
    void release();
    void save(int64_t now);
    void restore(Reason,int64_t now);
    void finish_restore(bool,int64_t now);
};
}
