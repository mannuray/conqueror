#include <unity.h>

#include "domain/ui/annunciator.h"
#include "domain/ui/catalogues.h"
#include "domain/ui/led_sequencer.h"
#include "domain/ui/sound_sequencer.h"

using namespace cq;

void setUp() {}
void tearDown() {}

void test_sound_plays_notes_in_order() {
  SoundSequencer s;
  const Note n[] = {{440, 100}, {0, 50}, {880, 100}};
  TEST_ASSERT_TRUE(s.play(n, 3, SoundSequencer::kPrioEvent, 0));
  uint16_t hz = 1;
  TEST_ASSERT_TRUE(s.tick(0, hz));
  TEST_ASSERT_EQUAL(440, hz);
  TEST_ASSERT_FALSE(s.tick(60, hz));
  TEST_ASSERT_TRUE(s.tick(100, hz));
  TEST_ASSERT_EQUAL(0, hz);
  TEST_ASSERT_TRUE(s.tick(150, hz));
  TEST_ASSERT_EQUAL(880, hz);
  TEST_ASSERT_TRUE(s.tick(250, hz));
  TEST_ASSERT_EQUAL(0, hz);
  TEST_ASSERT_FALSE(s.playing());
}

void test_sound_priority_and_modes() {
  SoundSequencer s;
  const Note a[] = {{1000, 500}};
  const Note b[] = {{2000, 100}};
  s.play(a, 1, SoundSequencer::kPrioAlarm, 0);
  TEST_ASSERT_FALSE(s.play(b, 1, SoundSequencer::kPrioEvent, 10));  // lower priority cannot interrupt
  s.stop();
  s.setMode(proto::AudioModeValue::kQuiet);
  TEST_ASSERT_FALSE(s.play(b, 1, SoundSequencer::kPrioEvent, 20));
  TEST_ASSERT_TRUE(s.play(a, 1, SoundSequencer::kPrioAlarm, 20));
  s.setMode(proto::AudioModeValue::kMute);
  uint16_t hz = 1;
  s.tick(30, hz);
  TEST_ASSERT_EQUAL(0, hz);  // mute silences a playing sound immediately
  TEST_ASSERT_FALSE(s.play(a, 1, SoundSequencer::kPrioAlarm, 40));
}

void test_led_layers_blink_and_expiry() {
  LedSequencer l;
  l.set(LedLayer::kBase, {proto::LedMode::kSolid, {0, 40, 0}, 0, 0}, 0);
  l.set(LedLayer::kCustom, {proto::LedMode::kBlink, {0, 0, 200}, 400, 1000}, 0);
  TEST_ASSERT_TRUE((l.output(100) == Rgb{0, 0, 200}));
  TEST_ASSERT_TRUE((l.output(300) == Rgb{0, 0, 0}));     // blink off half
  TEST_ASSERT_TRUE((l.output(1100) == Rgb{0, 40, 0}));   // custom expired → base
  l.set(LedLayer::kAlarm, {proto::LedMode::kSolid, {255, 0, 0}, 0, 0}, 1200);
  l.set(LedLayer::kCustom, {proto::LedMode::kSolid, {0, 0, 200}, 0, 0}, 1200);
  TEST_ASSERT_TRUE((l.output(1300) == Rgb{255, 0, 0}));  // alarm overrides custom
  l.clear(LedLayer::kAlarm);
  TEST_ASSERT_TRUE((l.output(1300) == Rgb{0, 0, 200}));
}

void test_led_pulse_peaks_mid_period() {
  LedSequencer l;
  l.set(LedLayer::kBase, {proto::LedMode::kPulse, {200, 100, 0}, 1000, 0}, 0);
  TEST_ASSERT_TRUE((l.output(500) == Rgb{200, 100, 0}));
  TEST_ASSERT_TRUE(l.output(0).r < 10);
  TEST_ASSERT_UINT8_WITHIN(5, 100, l.output(250).r);
}

void test_catalogue() {
  Note n[SoundSequencer::kMaxNotes];
  TEST_ASSERT_EQUAL(14, soundNotes(proto::SoundId::kStartup, n));  // old boot sweep
  TEST_ASSERT_EQUAL(200, n[0].hz);
  TEST_ASSERT_EQUAL(0, soundNotes(static_cast<proto::SoundId>(200), n));
}

namespace {
AnnState base() {
  AnnState s{};
  s.synced = true;
  s.owner = proto::Owner::kNone;
  s.hint = proto::OwnerHintValue::kNone;
  s.driving = true;  // steady base colour at full level (no breathing) unless a test says otherwise
  s.speed_pct = 100;
  return s;
}
}  // namespace

void test_annunciator_tilt_alarm_and_sound() {
  Annunciator a;
  LedSequencer l;
  SoundSequencer snd;
  AnnState s = base();
  a.update(s, 0, l, snd);
  s.tilted = true;
  a.update(s, 20, l, snd);
  TEST_ASSERT_TRUE(snd.playing());
  TEST_ASSERT_TRUE((l.output(20) == Rgb{255, 0, 0}) || (l.output(20) == Rgb{0, 0, 0}));  // red blink
  TEST_ASSERT_TRUE((l.output(20 + 60) == Rgb{255, 0, 0}));
}

void test_annunciator_owner_colours() {
  Annunciator a;
  LedSequencer l;
  SoundSequencer snd;
  AnnState s = base();
  s.owner = proto::Owner::kIr;
  a.update(s, 0, l, snd);
  TEST_ASSERT_TRUE((l.output(0) == Annunciator::kColorIr));
  s.owner = proto::Owner::kNetwork;
  s.hint = proto::OwnerHintValue::kAi;
  a.update(s, 20, l, snd);
  TEST_ASSERT_TRUE((l.output(20) == Annunciator::kColorAi));
}

void test_annunciator_parking_beeps_only_when_enabled() {
  Annunciator a;
  LedSequencer l;
  SoundSequencer snd;
  AnnState s = base();
  s.reversing = true;
  a.update(s, 0, l, snd);       // CHIRP_DOWN when reversing starts
  uint16_t hz = 0;
  for (uint32_t t = 0; t < 400; t += 20) snd.tick(t, hz);
  int beeps = 0;
  for (uint32_t t = 400; t < 3000; t += 20) {
    a.update(s, t, l, snd);
    if (snd.tick(t, hz) && hz != 0) ++beeps;
  }
  TEST_ASSERT_EQUAL(0, beeps);
  s.parking_beeps = true;
  for (uint32_t t = 3000; t < 6000; t += 20) {
    a.update(s, t, l, snd);
    if (snd.tick(t, hz) && hz != 0) ++beeps;
  }
  TEST_ASSERT_INT_WITHIN(1, 5, beeps);  // one TICK every 600 ms
}

void test_annunciator_alarm_follows_remaining_cause() {
  Annunciator a;
  LedSequencer l;
  SoundSequencer snd;
  AnnState s = base();
  s.lifted = true;
  s.batt_crit = true;
  a.update(s, 0, l, snd);
  TEST_ASSERT_TRUE((l.output(200) == Rgb{0, 0, 0}));  // 250 ms "lifted" blink: off half
  s.lifted = false;                                    // put down, battery still critical
  a.update(s, 1000, l, snd);
  TEST_ASSERT_TRUE((l.output(1200) == Annunciator::kColorAlarm));  // 1000 ms batt_crit blink: on half
  TEST_ASSERT_TRUE((l.output(1700) == Rgb{0, 0, 0}));              // off half
  s.selftest_critical = true;                                      // critical self-test failure → solid
  a.update(s, 2000, l, snd);
  TEST_ASSERT_TRUE((l.output(2700) == Annunciator::kColorAlarm));
}

// A flash (blink with flash_ms) is lit only for flash_ms at the start of each period; in between the layer is
// see-through, so the colour below (the base) shows (spec 02 LED table: "amber blink every 2 s over base colour").
void test_led_flash_shows_base_between_flashes() {
  LedSequencer l;
  l.set(LedLayer::kBase, {proto::LedMode::kSolid, {20, 20, 20}, 0, 0}, 0);
  l.set(LedLayer::kAlarm, {proto::LedMode::kBlink, {200, 120, 0}, 2000, 0, 150}, 0);
  TEST_ASSERT_TRUE((l.output(50) == Rgb{200, 120, 0}));    // flash
  TEST_ASSERT_TRUE((l.output(500) == Rgb{20, 20, 20}));    // base shows through
  TEST_ASSERT_TRUE((l.output(2050) == Rgb{200, 120, 0}));  // next flash
}

// Spec 02 LED table: no ESP32 link → blue breathing; battery low → short amber flash every 2 s over the base.
// (They were amber pulse and orange pulse: too alike to tell apart, owner 2026-10-14.)
void test_annunciator_no_link_is_blue() {
  Annunciator a;
  LedSequencer l;
  SoundSequencer snd;
  AnnState s = base();
  s.synced = false;
  a.update(s, 0, l, snd);
  TEST_ASSERT_TRUE((l.output(0) == Annunciator::kColorNoLink));
  TEST_ASSERT_EQUAL(0, Annunciator::kColorNoLink.r);
  TEST_ASSERT_EQUAL(0, Annunciator::kColorNoLink.g);
}

void test_annunciator_battery_low_is_short_amber_flash() {
  Annunciator a;
  LedSequencer l;
  SoundSequencer snd;
  AnnState s = base();
  s.batt_low = true;
  a.update(s, 0, l, snd);
  TEST_ASSERT_TRUE((l.output(50) == Annunciator::kColorLow));      // flash
  TEST_ASSERT_TRUE((l.output(1000) == Annunciator::kColorReady));  // base (ready) shows between flashes
  TEST_ASSERT_TRUE((l.output(2050) == Annunciator::kColorLow));
}

// LLD §10.6: the old-firmware melodies keep their exact lengths (docs/legacy/uno-old-led-buzzer.md §1).
void test_catalogue_old_melodies() {
  struct Expect {
    proto::SoundId id;
    uint8_t count;
    uint16_t total_ms;
  };
  const Expect e[] = {
      {proto::SoundId::kStartup, 14, 1100}, {proto::SoundId::kSuccess, 9, 445},    {proto::SoundId::kFanfare, 9, 550},
      {proto::SoundId::kSad, 6, 590},       {proto::SoundId::kSafetyChirp, 5, 160}, {proto::SoundId::kAckBlip, 2, 100},
      {proto::SoundId::kFail, 5, 710},      {proto::SoundId::kConfirm, 7, 255},    {proto::SoundId::kQuestion, 6, 390},
      {proto::SoundId::kOkChirp, 3, 100},   {proto::SoundId::kGrumble, 4, 260},    {proto::SoundId::kScan, 7, 350},
      {proto::SoundId::kGoodbye, 4, 390},
  };
  Note n[SoundSequencer::kMaxNotes];
  for (const Expect& x : e) {
    TEST_ASSERT_EQUAL(x.count, soundNotes(x.id, n));
    uint16_t total = 0;
    for (uint8_t i = 0; i < x.count; ++i) total = static_cast<uint16_t>(total + n[i].ms);
    TEST_ASSERT_EQUAL(x.total_ms, total);
  }
  soundNotes(proto::SoundId::kSafetyChirp, n);
  TEST_ASSERT_EQUAL(3000, n[2].hz);
}

void test_led_max_caps_every_layer() {
  LedSequencer l;
  l.setMax(50);
  l.set(LedLayer::kAlarm, {proto::LedMode::kSolid, {255, 0, 0}, 0, 0}, 0);
  TEST_ASSERT_TRUE((l.output(0) == Rgb{50, 0, 0}));
  l.clear(LedLayer::kAlarm);
  l.set(LedLayer::kCustom, {proto::LedMode::kSolid, {0, 0, 200}, 0, 0}, 0);
  TEST_ASSERT_TRUE((l.output(0) == Rgb{0, 0, 39}));
}

void test_led_base_breathes_idle_and_scales_with_speed() {
  LedSequencer l;
  l.set(LedLayer::kBase, {proto::LedMode::kSolid, {0, 200, 0}, 0, 0}, 0);
  l.setBaseStyle(true, 2000, 100);              // idle: breathe 20 % → 100 % → 20 % over 2 s
  TEST_ASSERT_EQUAL(40, l.output(0).g);         // 20 %
  TEST_ASSERT_EQUAL(200, l.output(1000).g);     // 100 % mid-period
  TEST_ASSERT_UINT8_WITHIN(2, 120, l.output(500).g);
  l.setBaseStyle(false, 2000, 20);              // driving at 0 %
  TEST_ASSERT_EQUAL(40, l.output(1000).g);
  l.setBaseStyle(false, 2000, 100);             // driving at full speed
  TEST_ASSERT_EQUAL(200, l.output(1000).g);
  l.set(LedLayer::kStatus, {proto::LedMode::kSolid, {0, 0, 100}, 0, 0}, 0);
  TEST_ASSERT_EQUAL(100, l.output(0).b);        // only the base layer is styled
}

void test_annunciator_obstacle_stop_chirp_and_blink() {
  Annunciator a;
  LedSequencer l;
  SoundSequencer snd;
  AnnState s = base();
  a.update(s, 0, l, snd);
  snd.stop();
  s.blocked = true;
  a.update(s, 20, l, snd);
  uint16_t hz = 0;
  TEST_ASSERT_TRUE(snd.tick(20, hz));
  TEST_ASSERT_EQUAL(2500, hz);                                     // safety chirp, once
  TEST_ASSERT_TRUE((l.output(20 + 50) == Annunciator::kColorAlarm));  // 100 ms on
  TEST_ASSERT_TRUE((l.output(20 + 150) == Rgb{0, 0, 0}));            // 100 ms off
  for (uint32_t t = 40; t < 1000; t += 20) {
    a.update(s, t, l, snd);
    snd.tick(t, hz);
  }
  TEST_ASSERT_FALSE(snd.playing());  // not repeated while blocked
}

void test_annunciator_override_pulse_and_toggle_blinks() {
  Annunciator a;
  LedSequencer l;
  SoundSequencer snd;
  AnnState s = base();
  a.update(s, 0, l, snd);
  s.override_on = true;
  ++s.override_seq;
  a.update(s, 100, l, snd);
  TEST_ASSERT_TRUE(snd.playing());                                       // chirp up
  TEST_ASSERT_TRUE((l.output(100 + 40) == Annunciator::kColorOn));        // green blink 1
  TEST_ASSERT_TRUE((l.output(100 + 120) == Rgb{0, 0, 0}));
  a.update(s, 600, l, snd);                                              // 480 ms later: pulse
  TEST_ASSERT_TRUE((l.output(600 + 500) == Annunciator::kColorOverride)); // pulse peak
  s.override_on = false;
  ++s.override_seq;
  a.update(s, 2000, l, snd);
  TEST_ASSERT_TRUE((l.output(2000 + 40) == Annunciator::kColorAlarm));    // red blink = off
  a.update(s, 2500, l, snd);
  TEST_ASSERT_TRUE((l.output(2500 + 500) == Annunciator::kColorReady));   // back to the base colour
  ++s.toggle_seq;                                                        // IR mute toggled on
  s.toggle_on = true;
  a.update(s, 3000, l, snd);
  TEST_ASSERT_TRUE((l.output(3000 + 40) == Annunciator::kColorOn));
}

void test_annunciator_override_pulse_returns_after_battery_check() {
  Annunciator a;
  LedSequencer l;
  SoundSequencer snd;
  AnnState s = base();
  a.update(s, 0, l, snd);
  s.override_on = true;
  ++s.override_seq;
  a.update(s, 100, l, snd);
  a.update(s, 600, l, snd);  // toggle blinks over → pulse
  l.set(LedLayer::kAttention, {proto::LedMode::kSolid, {0, 150, 0}, 0, 2000}, 1000);  // IR `9` battery check
  TEST_ASSERT_TRUE((l.output(1500) == Rgb{0, 150, 0}));
  l.output(3020);            // the 2 s pattern expires
  a.update(s, 3020, l, snd);  // Override still on → its pulse comes back
  TEST_ASSERT_TRUE((l.output(3020 + 500) == Annunciator::kColorOverride));
}

void test_annunciator_tilt_siren_repeats() {
  Annunciator a;
  LedSequencer l;
  SoundSequencer snd;
  AnnState s = base();
  a.update(s, 0, l, snd);
  s.tilted = true;
  uint16_t hz = 0;
  int starts = 0;
  for (uint32_t t = 20; t < 2020; t += 20) {
    a.update(s, t, l, snd);
    if (snd.tick(t, hz) && hz == 3000) ++starts;  // one 3000 Hz note per chirp
  }
  TEST_ASSERT_INT_WITHIN(1, 5, starts);  // one safety chirp every 400 ms
}

// Spec 02 §D6.3: USB power = the normal colour plus a short white flash every 2 s (no solid red).
void test_annunciator_usb_power_white_flash() {
  Annunciator a;
  LedSequencer l;
  SoundSequencer snd;
  AnnState s = base();
  s.usb_power = true;
  a.update(s, 0, l, snd);
  TEST_ASSERT_TRUE((l.output(50) == Annunciator::kColorUsb));
  TEST_ASSERT_TRUE((l.output(1000) == Annunciator::kColorReady));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_sound_plays_notes_in_order);
  RUN_TEST(test_sound_priority_and_modes);
  RUN_TEST(test_led_layers_blink_and_expiry);
  RUN_TEST(test_led_pulse_peaks_mid_period);
  RUN_TEST(test_led_flash_shows_base_between_flashes);
  RUN_TEST(test_annunciator_no_link_is_blue);
  RUN_TEST(test_annunciator_battery_low_is_short_amber_flash);
  RUN_TEST(test_annunciator_usb_power_white_flash);
  RUN_TEST(test_catalogue);
  RUN_TEST(test_annunciator_tilt_alarm_and_sound);
  RUN_TEST(test_annunciator_owner_colours);
  RUN_TEST(test_annunciator_parking_beeps_only_when_enabled);
  RUN_TEST(test_annunciator_alarm_follows_remaining_cause);
  RUN_TEST(test_catalogue_old_melodies);
  RUN_TEST(test_led_max_caps_every_layer);
  RUN_TEST(test_led_base_breathes_idle_and_scales_with_speed);
  RUN_TEST(test_annunciator_obstacle_stop_chirp_and_blink);
  RUN_TEST(test_annunciator_override_pulse_and_toggle_blinks);
  RUN_TEST(test_annunciator_override_pulse_returns_after_battery_check);
  RUN_TEST(test_annunciator_tilt_siren_repeats);
  return UNITY_END();
}
