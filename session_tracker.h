#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <list>

namespace click_sessions {

class Tracker {
public:
    static constexpr size_t MAX_ACTIVE=32;
    static constexpr size_t MAX_RETIRED=256;
    static constexpr uint64_t RETENTION_NS=10ull*60*1000*1000*1000;

    bool stale(uint64_t session,uint64_t command,uint64_t now) {
        prune(now);
        auto active=find(active_,session);
        if(active!=active_.end())return command<=active->highest;
        auto retired=find(retired_,session);
        return retired!=retired_.end()&&command<=retired->highest;
    }

    template<class Protected>
    bool admit(uint64_t session,uint64_t now,bool force_retirement,Protected protected_session) {
        prune(now);
        auto active=find(active_,session);
        if(active!=active_.end()) {
            active->stamp=now;active_.splice(active_.end(),active_,active);return true;
        }

        auto retired=find(retired_,session);uint64_t highest=0;
        if(retired!=retired_.end())highest=retired->highest;
        auto victim=active_.end();
        if(active_.size()>=MAX_ACTIVE) {
            victim=std::find_if(active_.begin(),active_.end(),[&](const Entry& entry){
                return !protected_session(entry.session);
            });
            if(victim==active_.end()) {
                if(!force_retirement)return false;
                victim=active_.begin();
            }
        }
        if(retired!=retired_.end())retired_.erase(retired);
        if(victim!=active_.end())retire(victim,now);
        active_.push_back({session,highest,now});return true;
    }

    void accept(uint64_t session,uint64_t command,uint64_t now) {
        auto active=find(active_,session);
        if(active==active_.end())return;
        active->highest=std::max(active->highest,command);active->stamp=now;
        active_.splice(active_.end(),active_,active);
    }

    size_t active_size()const{return active_.size();}
    size_t retired_size()const{return retired_.size();}

private:
    struct Entry {uint64_t session=0,highest=0,stamp=0;};
    std::list<Entry> active_,retired_;

    static std::list<Entry>::iterator find(std::list<Entry>& entries,uint64_t session) {
        return std::find_if(entries.begin(),entries.end(),[&](const Entry& entry){return entry.session==session;});
    }
    void prune(uint64_t now) {
        retired_.remove_if([&](const Entry& entry){
            return now>=entry.stamp&&now-entry.stamp>RETENTION_NS;
        });
    }
    void retire(std::list<Entry>::iterator active,uint64_t now) {
        auto prior=find(retired_,active->session);if(prior!=retired_.end())retired_.erase(prior);
        Entry entry=*active;entry.stamp=now;active_.erase(active);retired_.push_back(entry);
        while(retired_.size()>MAX_RETIRED)retired_.pop_front();
    }
};

}
