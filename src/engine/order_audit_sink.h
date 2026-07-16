#pragma once

#include "core/event.h"
#include "execution/order_tracker.h"  // for order_status

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#ifdef HAS_QUESTDB
#include "data/questdb/store.h"
#endif

// Full proposed interface. All methods use const char* where possible to avoid temporaries on hot paths.
// Matches QuestdbStore call sites 1:1 but hides overloads and QuestDB types from engine callers.

class IOrderAuditSink {
public:
    virtual ~IOrderAuditSink() = default;

    virtual void record_order_submitted(const order_event& o, const char* initial_status) = 0;
    virtual void record_status_transition(uint64_t order_id,
                                          order_status old_s,
                                          order_status new_s,
                                          const char* reason = nullptr) = 0;
    virtual void record_fill(const fill_event& f,
                             uint64_t opener_order_id,
                             const char* strategy_name,
                             const char* source) = 0;
    virtual void record_rejection(const order_event& o,
                                  const char* category,
                                  const char* detail) = 0;
    // Sparse variant: used for transport errors after async submit (no full order_event).
    virtual void record_rejection(uint64_t order_id,
                                  const char* symbol,
                                  const char* category,
                                  const char* detail) = 0;
    virtual void record_cancellation(uint64_t order_id,
                                     const char* symbol,
                                     const char* strategy_name,
                                     const char* reason) = 0;
    virtual void record_amendment(uint64_t order_id,
                                  const char* symbol,
                                  double old_price, double new_price,
                                  double old_qty, double new_qty,
                                  std::chrono::system_clock::time_point ts) = 0;
    virtual void record_funding(const funding_event& fe, const char* run_tag) = 0;
    virtual void record_event(const char* event_type,
                              const char* symbol,
                              const char* strategy_name,
                              uint64_t order_id,
                              const char* severity,
                              const char* message,
                              const char* details_json = "") = 0;

    virtual std::size_t total_rejections() const { return 0; }

    // Health: provide minimal struct for non-QuestDB builds; real one from QuestdbStore when enabled.
    struct Health {
        bool connected = false;
        std::size_t pending_lines = 0;
        std::size_t dropped_lines = 0;
        std::size_t fallback_lines = 0;
    };
    virtual Health health() const { return {}; }
};

class NoopOrderAuditSink final : public IOrderAuditSink {
public:
    void record_order_submitted(const order_event&, const char*) override {}
    void record_status_transition(uint64_t, order_status, order_status, const char*) override {}
    void record_fill(const fill_event&, uint64_t, const char*, const char*) override {}
    void record_rejection(const order_event&, const char*, const char*) override {}
    void record_rejection(uint64_t, const char*, const char*, const char*) override {}
    void record_cancellation(uint64_t, const char*, const char*, const char*) override {}
    void record_amendment(uint64_t, const char*, double, double, double, double, std::chrono::system_clock::time_point) override {}
    void record_funding(const funding_event&, const char*) override {}
    void record_event(const char*, const char*, const char*, uint64_t, const char*, const char*, const char*) override {}
    std::size_t total_rejections() const override { return 0; }
    Health health() const override { return {}; }
};

#ifdef HAS_QUESTDB
// Skeleton (full delegation in later PR when wiring; for now basic forwarding ready)
class QuestdbOrderAuditSink : public IOrderAuditSink {
public:
    explicit QuestdbOrderAuditSink(std::shared_ptr<truetest::questdb::QuestdbStore> store, bool* active_flag);

    void record_order_submitted(const order_event& o, const char* initial_status) override;
    void record_status_transition(uint64_t order_id,
                                  order_status old_s,
                                  order_status new_s,
                                  const char* reason = nullptr) override;
    void record_fill(const fill_event& f,
                     uint64_t opener_order_id,
                     const char* strategy_name,
                     const char* source) override;
    void record_rejection(const order_event& o,
                          const char* category,
                          const char* detail) override;
    void record_rejection(uint64_t order_id,
                          const char* symbol,
                          const char* category,
                          const char* detail) override;
    void record_cancellation(uint64_t order_id,
                             const char* symbol,
                             const char* strategy_name,
                             const char* reason) override;
    void record_amendment(uint64_t order_id,
                          const char* symbol,
                          double old_price, double new_price,
                          double old_qty, double new_qty,
                          std::chrono::system_clock::time_point ts) override;
    void record_funding(const funding_event& fe, const char* run_tag) override;
    void record_event(const char* event_type,
                      const char* symbol,
                      const char* strategy_name,
                      uint64_t order_id,
                      const char* severity,
                      const char* message,
                      const char* details_json = "") override;

    std::size_t total_rejections() const override;
    Health health() const override;

private:
    std::shared_ptr<truetest::questdb::QuestdbStore> store_;
    bool* active_flag_ = nullptr;
    std::size_t total_rejections_ = 0;
};
#endif
