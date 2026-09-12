#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>

namespace adsb {
// Session-only history. Bound both time and memory, including busy airspace.
class SessionHistory {
public:
    void reset(int hours) { entries_.clear(); bytes_=0; last_=0; capacity_=std::clamp(hours,1,24)*240; }
    void append(uint64_t now, const std::string& body) {
        if(!entries_.empty() && now>=last_ && now-last_<15000)return;
        if(now<last_) { entries_.clear(); bytes_=0; }
        last_=now;
        while(!entries_.empty() && (entries_.size()>=capacity_ || bytes_+body.size()>maxBytes_)) {
            bytes_-=entries_.front().size(); entries_.pop_front();
        }
        if(body.size()<=maxBytes_) { entries_.push_back(body); bytes_+=body.size(); }
    }
    size_t size() const { return entries_.size(); }
    const std::string& operator[](size_t i) const { return entries_[i]; }
private:
    std::deque<std::string> entries_;
    size_t bytes_=0, capacity_=1440;
    uint64_t last_=0;
    static constexpr size_t maxBytes_=512ULL*1024*1024;
};
}
