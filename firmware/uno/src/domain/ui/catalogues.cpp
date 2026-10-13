#include "domain/ui/catalogues.h"

#include <string.h>

#ifdef CQ_HOST_TEST
#define PROGMEM
#define CQ_READ_P(dst, src, n) memcpy((dst), (src), (n))
#else
#include <avr/pgmspace.h>
#define CQ_READ_P(dst, src, n) memcpy_P((dst), (src), (n))
#endif

namespace cq {
namespace {

// One flat note pool + a small index (LLD §10.6): a 14-note row does not make every row 14 notes long.
// Old-firmware melodies (docs/legacy/uno-old-led-buzzer.md §1) keep their exact notes; hz 0 = rest.
const Note kPool[] PROGMEM = {
    // 0 STARTUP = old melody 1, rising sweep + two-note tag (14)
    {200, 150}, {0, 80}, {500, 120}, {0, 60}, {1000, 100}, {0, 40}, {1500, 60}, {2000, 60}, {2500, 60}, {3000, 60},
    {0, 60}, {1500, 60}, {0, 40}, {2000, 150},
    // 14 READY (2)
    {784, 80}, {1047, 120},
    // 16 SELFTEST_FAIL (3)
    {400, 200}, {0, 80}, {300, 400},
    // 19 CHIRP_UP (3), 22 CHIRP_DOWN (3), 25 BLIP (1), 26 TICK (1)
    {1000, 40}, {1500, 40}, {2000, 40}, {2000, 40}, {1500, 40}, {1000, 40}, {1800, 30}, {2500, 15},
    // 27 JINGLE (5), 32 THINKING (5)
    {784, 120}, {659, 120}, {523, 120}, {659, 120}, {784, 240}, {600, 80}, {0, 80}, {700, 80}, {0, 80}, {800, 80},
    // 37 OUCH (2), 39 ERROR (3)
    {1200, 60}, {600, 150}, {300, 150}, {0, 60}, {300, 150},
    // 42 SUCCESS = old 2 (9)
    {800, 50}, {0, 25}, {1000, 50}, {0, 25}, {1400, 50}, {0, 25}, {1800, 80}, {0, 20}, {2200, 120},
    // 51 FANFARE = old 3 (9)
    {1200, 60}, {0, 30}, {1600, 60}, {0, 30}, {2000, 60}, {0, 50}, {2400, 80}, {0, 30}, {2400, 150},
    // 60 SAD = old 4 (6)
    {1500, 80}, {1200, 80}, {800, 100}, {0, 50}, {600, 80}, {400, 200},
    // 66 SAFETY_CHIRP = old 5 (5)
    {2500, 40}, {0, 20}, {3000, 40}, {0, 20}, {2500, 40},
    // 71 ACK_BLIP = old 6 (2)
    {1800, 40}, {1400, 60},
    // 73 FAIL = old 7 (5)
    {800, 150}, {0, 80}, {600, 150}, {0, 80}, {400, 250},
    // 78 CONFIRM = old 8 (7)
    {1500, 40}, {0, 20}, {2000, 40}, {0, 20}, {2500, 40}, {0, 15}, {2800, 80},
    // 85 QUESTION = old 9 (6)
    {800, 60}, {1200, 60}, {1600, 80}, {0, 30}, {1200, 60}, {2000, 100},
    // 91 OK_CHIRP = old 10 (3)
    {1500, 30}, {0, 20}, {2200, 50},
    // 94 GRUMBLE = old 11 (4)
    {300, 60}, {250, 60}, {300, 60}, {250, 80},
    // 98 SCAN = old 12 (7)
    {600, 50}, {1000, 50}, {1500, 50}, {2000, 50}, {1500, 50}, {1000, 50}, {600, 50},
    // 105 GOODBYE = old 13 (4)
    {1500, 80}, {1000, 80}, {600, 80}, {300, 150},
};  // 109 notes = 436 B

struct SoundIndex {
  uint8_t id;
  uint8_t offset;
  uint8_t count;
};

constexpr uint8_t id(proto::SoundId s) { return static_cast<uint8_t>(s); }

const SoundIndex kIndex[] PROGMEM = {
    {id(proto::SoundId::kStartup), 0, 14},    {id(proto::SoundId::kReady), 14, 2},
    {id(proto::SoundId::kSelftestFail), 16, 3}, {id(proto::SoundId::kChirpUp), 19, 3},
    {id(proto::SoundId::kChirpDown), 22, 3},  {id(proto::SoundId::kBlip), 25, 1},
    {id(proto::SoundId::kTick), 26, 1},       {id(proto::SoundId::kJingle), 27, 5},
    {id(proto::SoundId::kThinking), 32, 5},   {id(proto::SoundId::kOuch), 37, 2},
    {id(proto::SoundId::kError), 39, 3},      {id(proto::SoundId::kSuccess), 42, 9},
    {id(proto::SoundId::kFanfare), 51, 9},    {id(proto::SoundId::kSad), 60, 6},
    {id(proto::SoundId::kSafetyChirp), 66, 5}, {id(proto::SoundId::kAckBlip), 71, 2},
    {id(proto::SoundId::kFail), 73, 5},       {id(proto::SoundId::kConfirm), 78, 7},
    {id(proto::SoundId::kQuestion), 85, 6},   {id(proto::SoundId::kOkChirp), 91, 3},
    {id(proto::SoundId::kGrumble), 94, 4},    {id(proto::SoundId::kScan), 98, 7},
    {id(proto::SoundId::kGoodbye), 105, 4},
};  // 23 × 3 B = 69 B

static_assert(sizeof kPool / sizeof kPool[0] == 109, "pool offsets above assume 109 notes");

}  // namespace

uint8_t soundNotes(proto::SoundId sid, Note* out) {
  for (uint8_t i = 0; i < sizeof kIndex / sizeof kIndex[0]; ++i) {
    SoundIndex d;
    CQ_READ_P(&d, &kIndex[i], sizeof d);
    if (d.id != static_cast<uint8_t>(sid)) continue;
    CQ_READ_P(out, &kPool[d.offset], static_cast<size_t>(d.count) * sizeof(Note));
    return d.count;
  }
  return 0;
}

}  // namespace cq
