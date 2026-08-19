// Engine order lifecycle — Phase 3 domain-processor extraction complete:
// this file now holds only the public cancel_order()/modify_order() forward
// wrappers (kept because external callers, e.g. tests, call engine.cancel_
// order(...)/modify_order(...) directly). Every other former responsibility
// of this file (process_order, route_order, cancel/modify detail,
// resting-order triggering, exit firing, order/strategy attribution) now
// lives in OrderIntentProcessor — see order_intent_processor.{h,cpp} and the
// "OrderIntentProcessor Preparation Report" Phase 1/2/3 deliverables.
#include "engine.h"

// process_order / unwind_positions / deliver_mm_book_trades /
// resolve_instrument_spec / apply_instrument_spec / route_order /
// check_pending_stops / finalize_strategy_route /
// register_strategy_exit_intent / evaluate_exits (both overloads) /
// sweep_resting_limits moved to OrderIntentProcessor (Phase 1/2) — see
// order_intent_processor.cpp. Call sites now say orders_->process(...) /
// orders_->route(...) / etc.
//
// register_order_meta / lookup_opener / lookup_strategy_name forwards
// removed in Phase 3 (no remaining engine-side caller — see engine.h).
//
// notify_position_change_all moved to FillProcessor (Phase 2 engine
// decomposition, 2026-08) — see fills_->notify_position_change_all and
// core/docs/internal/engine-decomposition.md "Phase 2: Domain Processors".

bool engine::cancel_order(const std::string& symbol, uint64_t order_id,
                          const std::string& reason)
{
    return orders_->cancel(symbol, order_id, reason);
}

bool engine::modify_order(const std::string& symbol, uint64_t order_id,
                          double new_price, double new_qty)
{
    return orders_->modify(symbol, order_id, new_price, new_qty);
}
