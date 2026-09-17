#pragma once
#include "mouse_protocol.h"
#include <cstddef>
#include <cstdint>
#include <list>

namespace upx_replay {

struct Peer {
    uint32_t address=0;
    uint16_t port=0;
    bool operator==(const Peer& other)const {
        return address==other.address&&port==other.port;
    }
};

class Tracker {
public:
    static constexpr std::size_t MAX_PEERS=256;
    static constexpr uint64_t RETENTION_NS=10ull*60*1000*1000*1000;

    bool stale(const Peer& peer,uint32_t sequence,uint64_t now) {
        prune(now);
        auto entry=find(peer);
        return entry!=entries_.end()&&!sequence_newer(sequence,entry->sequence);
    }

    void accept(const Peer& peer,uint32_t sequence,uint64_t now) {
        prune(now);
        auto entry=find(peer);
        if(entry!=entries_.end()) {
            entry->sequence=sequence;entry->stamp=now;
            entries_.splice(entries_.end(),entries_,entry);return;
        }
        if(entries_.size()>=MAX_PEERS)entries_.pop_front();
        entries_.push_back({peer,sequence,now});
    }

    std::size_t size()const{return entries_.size();}

private:
    struct Entry {Peer peer{};uint32_t sequence=0;uint64_t stamp=0;};
    std::list<Entry> entries_;

    std::list<Entry>::iterator find(const Peer& peer) {
        auto entry=entries_.end();
        while(entry!=entries_.begin()) {
            --entry;if(entry->peer==peer)return entry;
        }
        return entries_.end();
    }

    void prune(uint64_t now) {
        while(!entries_.empty()) {
            const auto& oldest=entries_.front();
            if(now<oldest.stamp||now-oldest.stamp<=RETENTION_NS)break;
            entries_.pop_front();
        }
    }
};

}
