#include "session_tracker.h"
#include <cassert>
#include <iostream>

int main() {
    using click_sessions::Tracker;Tracker tracker;uint64_t now=1;
    auto none=[](uint64_t){return false;};
    for(uint64_t session=1;session<=Tracker::MAX_ACTIVE;++session) {
        assert(tracker.admit(session,now++,false,none));tracker.accept(session,10,now++);
    }
    assert(tracker.active_size()==Tracker::MAX_ACTIVE&&tracker.retired_size()==0);

    assert(tracker.admit(33,now++,false,none));tracker.accept(33,10,now++);
    assert(tracker.active_size()==Tracker::MAX_ACTIVE&&tracker.retired_size()==1);
    assert(tracker.stale(1,9,now)&&tracker.stale(1,10,now));
    assert(tracker.admit(1,now++,false,none));tracker.accept(1,11,now++);
    assert(!tracker.stale(1,12,now));

    auto all=[](uint64_t){return true;};
    assert(!tracker.admit(34,now++,false,all));
    assert(!tracker.stale(34,10,now)); // QueueFull did not reserve a high-water mark.
    assert(tracker.admit(34,now++,true,all));tracker.accept(34,10,now++); // ReleaseAll can enter.

    for(uint64_t session=35;session<1000;++session) {
        assert(tracker.admit(session,now++,true,all));tracker.accept(session,1,now++);
        assert(tracker.active_size()<=Tracker::MAX_ACTIVE);
        assert(tracker.retired_size()<=Tracker::MAX_RETIRED);
    }
    assert(tracker.stale(900,1,now));
    now+=Tracker::RETENTION_NS+1;
    assert(!tracker.stale(900,1,now)); // Retired replay protection has an explicit lifetime.
    std::cout<<"Session LRU, retirement, replay tombstones, release admission and expiry passed\n";
}
