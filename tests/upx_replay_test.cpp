#include "upx_replay.h"
#include <cassert>
#include <iostream>

using upx_replay::Peer;
using upx_replay::Tracker;

int main() {
    uint64_t now=1000;
    Tracker ordering;Peer original{0x0100007fu,40000};
    assert(!ordering.stale(original,100,now));
    assert(ordering.size()==0); // Looking at an unknown peer does not allocate or reset history.
    ordering.accept(original,100,now++);
    assert(ordering.stale(original,100,now));
    assert(ordering.stale(original,99,now));
    assert(!ordering.stale(original,101,now));
    ordering.accept(original,101,now++);
    now+=300ull*1000*1000; // A 250 ms ownership timeout does not expire replay state.
    assert(ordering.stale(original,100,now));

    Peer restarted{original.address,40001};
    assert(!ordering.stale(restarted,0x8badf00du,now));
    ordering.accept(restarted,0x8badf00du,now++);
    assert(ordering.stale(restarted,0x8badf00cu,now));

    Tracker wrapping;Peer wrap_peer{0x0200007fu,41000};
    wrapping.accept(wrap_peer,0xfffffffeu,now++);
    assert(!wrapping.stale(wrap_peer,0xffffffffu,now));
    wrapping.accept(wrap_peer,0xffffffffu,now++);
    assert(!wrapping.stale(wrap_peer,0,now));
    wrapping.accept(wrap_peer,0,now++);
    assert(!wrapping.stale(wrap_peer,1,now));
    assert(wrapping.stale(wrap_peer,0xffffffffu,now));
    assert(wrapping.stale(wrap_peer,0x80000000u,now)); // Exactly half-range is ambiguous.

    Tracker bounded;
    for(std::size_t i=0;i<Tracker::MAX_PEERS;++i)
        bounded.accept(Peer{uint32_t(i+1),uint16_t(10000+i)},100,uint64_t(i+1));
    assert(bounded.size()==Tracker::MAX_PEERS);
    Peer kept{1,10000},evicted{2,10001};
    bounded.accept(kept,101,1000); // Refresh the oldest entry before adding another peer.
    bounded.accept(Peer{999,19999},1,1001);
    assert(bounded.size()==Tracker::MAX_PEERS);
    assert(bounded.stale(kept,100,1002));
    assert(!bounded.stale(evicted,100,1002));

    Tracker expiry;Peer expiring{3,42000};
    expiry.accept(expiring,500,now);
    assert(expiry.stale(expiring,499,now+Tracker::RETENTION_NS));
    assert(!expiry.stale(expiring,499,now+Tracker::RETENTION_NS+1));
    assert(expiry.size()==0);

    std::cout<<"UPX1 per-peer ordering, wraparound, bounded capacity and expiry passed\n";
}
