#pragma once
#include <cerrno>
#include <cstdint>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

class SchedulerEvent {
    int fd_ = eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
public:
    SchedulerEvent() = default;
    SchedulerEvent(const SchedulerEvent&) = delete;
    SchedulerEvent& operator=(const SchedulerEvent&) = delete;
    ~SchedulerEvent(){if(fd_>=0)close(fd_);}
    int fd() const {return fd_;}
    void signal() const {
        const uint64_t one=1;
        // A saturated counter is already readable.
        while(write(fd_,&one,sizeof(one))<0&&errno==EINTR){}
    }
};

inline void consume_scheduler_fd(int fd) {
    uint64_t count;
    while(read(fd,&count,sizeof(count))<0&&errno==EINTR){}
}

class SchedulerTimer {
    int fd_ = timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK|TFD_CLOEXEC);
    uint64_t deadline_ = UINT64_MAX;
public:
    SchedulerTimer() = default;
    SchedulerTimer(const SchedulerTimer&) = delete;
    SchedulerTimer& operator=(const SchedulerTimer&) = delete;
    ~SchedulerTimer(){if(fd_>=0)close(fd_);}
    int fd() const {return fd_;}
    bool arm(uint64_t deadline) {
        if(deadline==deadline_)return true;
        itimerspec time{};
        if(deadline!=UINT64_MAX) {
            if(!deadline)deadline=1; // Zero disarms timerfd.
            time.it_value.tv_sec=deadline/1000000000;
            time.it_value.tv_nsec=deadline%1000000000;
        }
        if(timerfd_settime(fd_,TFD_TIMER_ABSTIME,&time,nullptr)<0)return false;
        deadline_=deadline;
        return true;
    }
    void consume() {
        consume_scheduler_fd(fd_);
        deadline_=UINT64_MAX;
    }
};
