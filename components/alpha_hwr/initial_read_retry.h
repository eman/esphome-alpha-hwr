#pragma once

#include <cstdint>

namespace esphome {
namespace alpha_hwr {
namespace core {

/**
 * Re-arm predicate for the one-shot initial data read chain.
 *
 * trigger_initial_data_reads() latches initial_data_read_done_, and the only
 * thing that clears it is a BLE disconnect. That is fine when the chain runs
 * after a fresh handshake, but the chain also runs from update() when the link
 * persists through an ESP32 restart and there is no re-auth. On that path it
 * can fire before the pump is answering: the reads fail, the latch stays set,
 * and nothing ever retries. The device then stays half-initialised for as long
 * as the link stays up -- telemetry keeps streaming, so it looks alive, but
 * device info and the operating statistics are never read.
 *
 * This decides when to abandon an attempt and run the chain again.
 */
inline bool should_rearm_initial_read(bool session_ready, bool chain_done,
                                      bool caches_synchronized,
                                      bool chain_products_complete,
                                      uint32_t now_ms,
                                      uint32_t attempt_started_ms,
                                      uint32_t timeout_ms) {
  // Only an attempt that was actually made can be retried, and only while the
  // session is up -- a disconnect clears the latch on its own path, so
  // re-arming here would race it.
  if (!session_ready || !chain_done) {
    return false;
  }

  // Success takes BOTH terms, and the second one is the whole point.
  //
  // is_state_synchronized() alone is not a success condition for this chain,
  // because neither of its cache terms depends on the chain landing: the
  // schedule overview is refreshed by the 10 s poll in update(), and the
  // control cache sync re-schedules itself every 5 s until it succeeds. Both
  // therefore heal on their own once the pump starts answering. Retrying only
  // until the caches are valid would stop the moment those two self-healing
  // paths recovered -- typically well inside the timeout -- while the reads
  // this exists to retry stay unread until the next disconnect, which is the
  // exact symptom being fixed.
  if (caches_synchronized && chain_products_complete) {
    return false;
  }

  // Unsigned subtraction is rollover-correct across the ~49-day millis() wrap.
  return (now_ms - attempt_started_ms) >= timeout_ms;
}

/**
 * Backoff for successive re-arms.
 *
 * The re-arm is unbounded by design -- capping the retry count would restore
 * the permanent stall it exists to prevent -- so the interval has to grow
 * instead, or a pump that never returns one of these values would be re-read
 * forever at a fixed rate. Doubling from 60 s to a 10 min ceiling keeps
 * recovery quick in the case that motivated this (a pump answering a few
 * seconds late) while settling to a background rate that costs almost nothing
 * on a pump that never will.
 */
inline uint32_t next_initial_read_backoff_ms(uint32_t current_ms,
                                             uint32_t max_ms) {
  const uint32_t doubled = current_ms * 2u;
  // Guard the multiply as well as the ceiling: overflow would wrap to a tiny
  // interval and produce exactly the storm the backoff exists to prevent.
  if (doubled < current_ms || doubled > max_ms) {
    return max_ms;
  }
  return doubled;
}

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
