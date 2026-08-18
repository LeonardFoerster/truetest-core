#pragma once

#include "private_execution_record.h"

// Execution-layer port for the authoritative provider-owned handoff.  The
// concrete fixed SPSC ring belongs to providers/ because it depends on the
// threading implementation; ExecutionBridge needs only this tiny no-throw
// publish/consume contract and must not pull the provider/threading layer
// inward.
class IPrivateExecutionIngress
{
public:
    virtual ~IPrivateExecutionIngress() = default;

    // The private reader is the sole publisher.  The implementation assigns a
    // strictly increasing sequence and latches failure instead of replacing a
    // record when its fixed capacity is exhausted.
    virtual bool try_publish(private_execution_record& record) noexcept = 0;
    virtual bool try_pop(private_execution_record& record) noexcept = 0;
    virtual bool empty() const noexcept = 0;
    virtual bool failed() const noexcept = 0;
    virtual void latch_failure() noexcept = 0;
};
