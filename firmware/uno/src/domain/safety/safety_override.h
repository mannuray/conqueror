#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {

// Safety Override (LLD §10, spec 02 §A6.1): a bounded human-only mode that switches the obstacle brake off and
// raises the tilt cut-off. Deadman/link loss, battery critical, stall cut, lift and STOP never consult it.
class SafetyOverride {
 public:
  enum class Binder : uint8_t { kNone, kIr, kNetwork };  // who enabled it
  struct Gate {
    bool synced;           // LinkSession::synced()
    bool child_play;       // Envelope::childPlay(): only child play refuses Override (adult envelopes allowed)
    bool fault_latched;    // tilted / lifted / battery critical / self-test lock: Override never un-halts
    proto::Owner owner;    // Arbitrator::owner()
  };
  struct Config {
    uint8_t idle_s;   // override_idle_s
    uint16_t max_s;   // override_max_s
  };

  // SAFETY_OVERRIDE frame. on = 0 is accepted from any source.
  proto::AckStatus requestNetwork(bool on, proto::Source src, const Gate& g, uint32_t now_ms);
  // IR `*` held 2 s: toggles. The caller has already made IR the owner.
  proto::AckStatus requestIr(const Gate& g, uint32_t now_ms);
  void onOwner(proto::Owner owner);                                      // every owner change
  void onNetworkMotion(proto::Source src, bool driving, uint32_t now_ms);  // accepted network motion frame
  void onIrMotion(bool driving, uint32_t now_ms);                       // accepted IR motion key
  void onOwnerHint(proto::OwnerHintValue hint);                         // OWNER_HINT ai ends it
  void onLinkLost();                                                    // ends a network-bound Override
  void end(proto::OverrideReason why);                                  // STOP, envelope, …
  void tick(uint32_t now_ms, const Config& cfg);                        // idle / max expiry

  bool active() const { return m_binder != Binder::kNone; }
  Binder binder() const { return m_binder; }
  // Effective obstacle brake (supervisor + TargetLimiter) and tilt limit (LiftDetector).
  bool brakeOn(bool configured) const { return configured && !active(); }
  uint8_t tiltLimit(uint8_t normal, uint8_t relaxed) const {
    return (active() && relaxed > normal) ? relaxed : normal;
  }
  bool takeEvent(bool& on, proto::OverrideReason& why);

 private:
  proto::AckStatus enter(Binder b, proto::OverrideReason why, uint32_t now_ms);
  void post(bool on, proto::OverrideReason why);

  Binder m_binder = Binder::kNone;
  uint32_t m_since_ms = 0;
  uint32_t m_last_drive_ms = 0;
  bool m_ev_pending = false;
  bool m_ev_on = false;
  proto::OverrideReason m_ev_why = proto::OverrideReason::kNetwork;
};

}  // namespace cq
