#pragma once
#include "move_protocol.h"
#include "mouse_mixer.h"
#include <algorithm>

// Single synthetic output owner. Driven only by the UDP thread and writer events.
class MoveExecutor {
    move_protocol::Request request_{};
    move_protocol::Ack ack_{};
    uint64_t due_ = 0, deadline_ = 0, activity_ = 0;
    bool pending_ = false, continued_ = false, cancelled_ = false;
    void stage(move_protocol::Stage value, uint64_t now) {
        ack_.stage = value;
        ack_.written_ns = now;
    }

  public:
    bool active() const {
        return ack_.status == move_protocol::Status::Active || ack_.status == move_protocol::Status::Accepted;
    }
    const move_protocol::Ack& ack() const {
        return ack_;
    }
    void start(const move_protocol::Request& r, const EndpointSnapshot& e, uint64_t now) {
        request_ = r;
        ack_ = {};
        ack_.status = move_protocol::Status::Active;
        ack_.stage = move_protocol::Stage::Out;
        ack_.session = r.session;
        ack_.command = r.command;
        ack_.epoch = r.epoch;
        ack_.generation = r.generation;
        ack_.written_ns = now;
        pending_ = continued_ = cancelled_ = false;
        deadline_ = now + uint64_t(r.budget_us) * 1000;
        due_ = now;
        activity_ = e.activity;
    }
    void stop(move_protocol::Status reason, uint64_t now) {
        if (!active())
            return;
        if (pending_)
            ack_.flags |= 1;
        ack_.status = reason;
        stage(move_protocol::Stage::Done, now);
        pending_ = false;
    }
    void proceed() {
        if (active() && ack_.stage == move_protocol::Stage::Waiting)
            continued_ = true;
    }
    void cancel() {
        if (active())
            cancelled_ = true;
    }
    void event(const WriterEvent& e) {
        using namespace move_protocol;
        if (!active() || e.session != request_.session || e.command != request_.command ||
            e.generation != request_.generation)
            return;
        if (!e.success) {
            stop(Status::WriterFailed, e.written_ns);
            return;
        }
        if (e.kind == REPORT_TRANSACTION_OUT) {
            ack_.out_x += e.dx;
            ack_.out_y += e.dy;
        } else if (e.kind == REPORT_TRANSACTION_RETURN) {
            ack_.return_x += e.dx;
            ack_.return_y += e.dy;
        } else if (e.kind != REPORT_TRANSACTION_PRESS && e.kind != REPORT_TRANSACTION_RELEASE)
            return;
        ack_.residual_x = ack_.out_x + ack_.return_x;
        ack_.residual_y = ack_.out_y + ack_.return_y;
        ack_.written_ns = e.written_ns;
        if (!e.finished)
            return;
        pending_ = false;
        if (e.kind == REPORT_TRANSACTION_OUT) {
            stage(Stage::Settle, e.written_ns);
            due_ = e.written_ns + uint64_t(request_.settle_us) * 1000;
        } else if (e.kind == REPORT_TRANSACTION_PRESS) {
            if (e.blocked) {
                stop(Status::ButtonActive, e.written_ns);
                return;
            }
            ack_.flags |= 2;
            stage(Stage::Hold, e.written_ns);
            due_ = e.written_ns + uint64_t(request_.press_us) * 1000;
        } else if (e.kind == REPORT_TRANSACTION_RELEASE) {
            ack_.flags |= 4;
            stage(Stage::Delay, e.written_ns);
            due_ = e.written_ns + uint64_t(request_.return_us) * 1000;
        } else {
            ack_.status = cancelled_ ? Status::Cancelled : Status::Completed;
            stage(Stage::Done, e.written_ns);
        }
    }
    // True requests the server's existing final-release/reset obligation.
    bool tick(const EndpointSnapshot& e, uint64_t now) {
        using namespace move_protocol;
        if (!active())
            return false;
        if (!e.ready || e.generation != request_.generation) {
            stop(Status::EndpointChanged, now);
            return true;
        }
        if (e.activity != activity_) {
            stop(Status::Interrupted, now);
            return true;
        }
        if (now >= deadline_) {
            stop(Status::Timeout, now);
            return true;
        }
        if (pending_)
            return false;
        if (cancelled_ && ack_.stage != Stage::Return) {
            if ((ack_.flags & 2) && !(ack_.flags & 4))
                stage(Stage::Release, now);
            else {
                stage(Stage::Delay, now);
                due_ = now;
            }
        }
        MouseCommand command;
        ReportKind kind = REPORT_NONE;
        uint8_t scheduled = 0;
        switch (ack_.stage) {
        case Stage::Out:
            if (!request_.dx && !request_.dy) {
                stage(Stage::Settle, now);
                due_ = now + uint64_t(request_.settle_us) * 1000;
                return false;
            }
            command.x = request_.dx;
            command.y = request_.dy;
            kind = REPORT_TRANSACTION_OUT;
            break;
        case Stage::Settle:
            if (now < due_)
                return false;
            if (request_.flags & 1) {
                stage(Stage::Waiting, now);
                due_ = now + uint64_t(request_.wait_us) * 1000;
                return false;
            }
            stage(request_.button ? Stage::Press : Stage::Delay, now);
            due_ = now + uint64_t(request_.return_us) * 1000;
            return false;
        case Stage::Waiting:
            if (!continued_ && now < due_)
                return false;
            if (!continued_)
                cancelled_ = true;
            stage(!cancelled_ && request_.button ? Stage::Press : Stage::Delay, now);
            due_ = now;
            return false;
        case Stage::Press:
            kind = REPORT_TRANSACTION_PRESS;
            scheduled = uint8_t(1u << (request_.button - 1));
            break;
        case Stage::Hold:
            if (now < due_)
                return false;
            stage(Stage::Release, now);
            return false;
        case Stage::Release:
            kind = REPORT_TRANSACTION_RELEASE;
            break;
        case Stage::Delay:
            if (now < due_)
                return false;
            if (!ack_.residual_x && !ack_.residual_y) {
                ack_.status = cancelled_ ? Status::Cancelled : Status::Completed;
                stage(Stage::Done, now);
                return false;
            }
            stage(Stage::Return, now);
            return false;
        case Stage::Return:
            command.x = -ack_.residual_x;
            command.y = -ack_.residual_y;
            kind = REPORT_TRANSACTION_RETURN;
            break;
        default:
            return false;
        }
        const auto result =
            enqueue_report(command, 0, scheduled, kind, false, request_.session, request_.command, 0,
                           request_.button, activity_, request_.generation);
        if (result == EnqueueResult::Ok)
            pending_ = true;
        else if (result != EnqueueResult::QueueFull) {
            stop(Status::EndpointChanged, now);
            return true;
        }
        return false;
    }
};
