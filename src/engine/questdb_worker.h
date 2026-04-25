#pragma once
#ifdef HAS_QUESTDB

#include "../core/event.h"
#include "../data/questdb/store.h"
#include "../threading/worker.h"

#include <atomic>
#include <memory>

class QuestDbWorker : public Worker
{
public:
    explicit QuestDbWorker(std::shared_ptr<truetest::questdb::QuestdbStore> store)
        : store_(std::move(store))
    {}

    const char* worker_name() const override { return "questdb"; }

    void on_event(const event_pointer& ev) override
    {
        if (!store_ || !ev) return;
        events_seen_.fetch_add(1, std::memory_order_relaxed);

        switch (ev->get_type())
        {
        case event_type::order:
        {
            const auto& o = static_cast<const order_event&>(*ev);
            store_->record_order_submitted(o, "pending");
            break;
        }
        case event_type::fill:
        {
            const auto& f = static_cast<const fill_event&>(*ev);
            const char* src = (f.get_source() == fill_source::exchange) ? "exchange"
                            : (f.get_source() == fill_source::simulated) ? "simulated"
                            : "unknown";
            store_->record_fill(f, /*opener=*/0, /*strategy=*/"", src);
            break;
        }
        case event_type::cancel:
        {
            const auto& c = static_cast<const cancel_event&>(*ev);
            store_->record_cancellation(c.get_order_id(), c.get_symbol(),
                                        /*strategy=*/"", c.get_reason());
            break;
        }
        case event_type::amend:
        {
            const auto& a = static_cast<const amend_event&>(*ev);
            // old_price/old_qty are not on the event; engine path supplies
            // those at the synchronous capture point (Step 7). The ring
            // path here records the new values only with zeros for old.
            store_->record_amendment(a.get_order_id(), a.get_symbol(),
                                     /*old_price=*/0.0, a.get_new_price(),
                                     /*old_qty=*/0.0, a.get_new_quantity(),
                                     a.get_timestamp());
            break;
        }
        case event_type::rejection:
        {
            const auto& r = static_cast<const rejection_event&>(*ev);
            // The ring rejection event lacks side/qty/price; build a
            // throwaway order_event for the API (engine supplies the rich
            // version synchronously in Step 7).
            order_event stub(r.get_timestamp(), r.get_symbol(),
                             order_type::market, order_side::buy,
                             0.0, 0.0);
            stub.set_order_id(r.get_order_id());
            store_->record_rejection(stub, /*category=*/"unknown",
                                     r.get_reason());
            break;
        }
        // Skip uncaptured event types (market, signal, tick, l2_*).
        default:
            break;
        }
        store_->tick();
    }

    std::size_t events_seen() const
    {
        return events_seen_.load(std::memory_order_relaxed);
    }

private:
    std::shared_ptr<truetest::questdb::QuestdbStore> store_;
    std::atomic<std::size_t> events_seen_{0};
};

#endif // HAS_QUESTDB
