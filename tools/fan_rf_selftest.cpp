// Host self-test for the on-device decoder.
//
// Compiles the exact header the firmware uses -- fan_rf/fan_rf_protocol.h,
// which carries all three stages -- so the decoder can be validated before
// anything is flashed.
//
//   c++ -std=c++17 -O2 -o /tmp/fan_rf_selftest tools/fan_rf_selftest.cpp
//   /tmp/fan_rf_selftest                   # unit + synthetic burst checks
//   /tmp/fan_rf_selftest log.txt           # replay a captured `dump: raw` log
//   /tmp/fan_rf_selftest --dump log.txt    # reproduce the dump_frames output
//
// No capture log is needed for the checks. The frame layer is already known
// good against real captures, and the protocol is fully decoded in PROTOCOL.md,
// so everything above the frame layer is verified against synthesised bursts
// built from that document: the key table, the checksum, and the press/repeat
// state machine, end to end through the same walk_frames() the firmware calls.
//
// The transmit path is checked the same way round. Its output is fed straight
// back into the receive path, which is the validated one -- if walk_frames()
// and decode_packet() pull the intended key and counter out of what the
// builders emitted, the waveform is right. The literal durations are asserted
// against PROTOCOL.md separately, because a round trip alone would pass on any
// self-consistent pair of timings.
//
// The log modes remain because a real capture is still the only way to find out
// what the receiver did on a given evening. They are diagnostics, not the test.

#include "../firmware/components/fan_rf/fan_rf_protocol.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace smarterfan::fan_rf;

// --- test scaffolding -------------------------------------------------------

static int failures = 0;
static int checks = 0;

static void check(bool condition, const char *what) {
  checks++;
  if (!condition) {
    printf("FAIL: %s\n", what);
    failures++;
  }
}

static void checkf(bool condition, const char *fmt, ...) {
  checks++;
  if (condition)
    return;
  va_list args;
  va_start(args, fmt);
  printf("FAIL: ");
  vprintf(fmt, args);
  printf("\n");
  va_end(args);
  failures++;
}

static std::string to_bits(uint32_t value) {
  std::string out(32, '0');
  for (int i = 0; i < 32; i++)
    out[i] = (value & (1u << (31 - i))) ? '1' : '0';
  return out;
}

// --- burst synthesis --------------------------------------------------------
//
// Build the (mark, space) list the RMT peripheral would hand over, from the
// timings in PROTOCOL.md section 1. `skew` is the receiver's AGC bias: it
// stretches every mark and eats the same amount from every space, which is
// exactly what the real receiver does while it settles (+89us in frame 1,
// +22us once settled). It cancels in mark+space, so the period check should be
// blind to it -- proving that is half the point of this test.

static void append_frame(std::vector<int32_t> &out, uint32_t code, int32_t skew) {
  for (int i = PACKET_BITS - 1; i >= 0; i--) {
    const bool one = (code >> i) & 1u;
    const int32_t mark = (int32_t) (one ? LONG_US : SHORT_US) + skew;
    const int32_t space = (int32_t) (one ? SHORT_US : LONG_US) - skew;
    out.push_back(mark);
    out.push_back(-space);
  }
  out.push_back(330);    // stop mark
  out.push_back(-8787);  // inter-frame gap
}

// A whole burst: the lone preamble mark, then `frames` repeats of one codeword.
// `damaged` frames (1-based, 0 = none) get one symbol corrupted, which is what
// the AGC does when it compresses a space below the glitch filter.
static std::vector<int32_t> make_burst(uint32_t code, int frames, int damaged = 0) {
  std::vector<int32_t> out;
  out.push_back(335);
  out.push_back(-7690);
  for (int f = 1; f <= frames; f++) {
    const size_t before = out.size();
    // Frame 1 carries the full settling skew, later frames the settled value.
    append_frame(out, code, f == 1 ? 89 : 22);
    if (f == damaged)
      out[before + 8] = 2000;  // a mark no bit width or period can accept
  }
  return out;
}

// Drive one capture through the component's path: split into frames, validate
// each, feed the tracker. Returns the (repeat index) of every emitted event.
static std::vector<uint32_t> run_capture(PressTracker &tracker, const std::vector<int32_t> &raw,
                                         uint32_t now_ms, Packet *last = nullptr,
                                         FrameWalk *walk_out = nullptr) {
  std::vector<uint32_t> events;
  const FrameWalk walk = walk_frames(raw.data(), raw.size(), [&](uint32_t bits) {
    const Packet p = decode_packet(bits);
    if (!p.valid())
      return;
    if (last != nullptr)
      *last = p;
    const PressEvent e = tracker.feed(p.raw, now_ms);
    if (e.emit)
      events.push_back(e.repeat);
  });
  if (walk_out != nullptr)
    *walk_out = walk;
  return events;
}

// --- the key table, transcribed from PROTOCOL.md section 4 ------------------
//
// Independent of the encoder: these are the codewords as captured, so they
// check encode_packet() and decode_packet() against measurement rather than
// against each other.

struct KeyRow {
  uint8_t key;
  const char *name;
  uint32_t cnt0_frame;
};

static const KeyRow KEY_TABLE[] = {
    {3, "bright+", 0xA1D8218F},           {4, "fan forward", 0xA1D82204},
    {5, "bright-", 0xA1D8228C},           {6, "all off", 0xA1D82305},
    {7, "temp+", 0xA1D8238D},             {8, "light on/off", 0xA1D82402},
    {9, "2H", 0xA1D8248A},                {10, "fan 4", 0xA1D82503},
    {11, "temp-", 0xA1D8258B},            {12, "fan 6", 0xA1D82600},
    {13, "cycle full bright", 0xA1D82688}, {15, "fan 5", 0xA1D82789},
    {16, "fan 1", 0xA1D8280E},            {17, "fan reverse", 0xA1D82886},
    {18, "fan 2", 0xA1D8290F},            {19, "night mode", 0xA1D82987},
    {21, "natural wind", 0xA1D82A84},     {22, "fan off", 0xA1D82B0D},
    {25, "4H", 0xA1D82C82},               {28, "fan 3", 0xA1D82E08},
};
static const size_t KEY_COUNT = sizeof(KEY_TABLE) / sizeof(KEY_TABLE[0]);

// --- checks -----------------------------------------------------------------

static void check_key_table() {
  printf("-- key table: %zu buttons --\n", KEY_COUNT);
  for (size_t i = 0; i < KEY_COUNT; i++) {
    const KeyRow &row = KEY_TABLE[i];
    const Packet p = decode_packet(row.cnt0_frame);
    checkf(p.valid(), "key %u (%s): captured frame validates", row.key, row.name);
    checkf(p.key == row.key, "key %u (%s): decodes to key %u", row.key, row.name, p.key);
    checkf(p.counter == 0, "key %u (%s): decodes to counter 0", row.key, row.name);
    checkf(strcmp(key_name(row.key), row.name) == 0, "key %u names as \"%s\", got \"%s\"", row.key,
           row.name, key_name(row.key));
    checkf(encode_packet(row.key, 0) == row.cnt0_frame,
           "key %u (%s): encode_packet gives 0x%08X, captured 0x%08X", row.key, row.name,
           encode_packet(row.key, 0), row.cnt0_frame);

    // All eight counter values for this button must validate, and each must
    // decode back to the same key -- the counter must not bleed into the key.
    for (uint8_t c = 0; c < 8; c++) {
      const Packet q = decode_packet(encode_packet(row.key, c));
      checkf(q.valid() && q.key == row.key && q.counter == c,
             "key %u (%s) counter %u round-trips", row.key, row.name, c);
    }
  }

  // No two buttons may share a codeword. This is what the old command-byte
  // model got wrong: it dropped KEY's low bit, so `bright-`/`all off` and
  // `fan 6`/`cycle full bright` collided.
  for (size_t i = 0; i < KEY_COUNT; i++)
    for (size_t j = i + 1; j < KEY_COUNT; j++)
      checkf(KEY_TABLE[i].cnt0_frame != KEY_TABLE[j].cnt0_frame,
             "keys %u and %u must not share a codeword", KEY_TABLE[i].key, KEY_TABLE[j].key);

  // Every unlisted KEY still validates when its checksum is right -- unnamed is
  // not invalid -- but must never be named as a real button.
  for (uint8_t k = 0; k < 32; k++) {
    bool listed = false;
    for (size_t i = 0; i < KEY_COUNT; i++)
      listed = listed || KEY_TABLE[i].key == k;
    if (listed)
      continue;
    checkf(decode_packet(encode_packet(k, 3)).valid(), "unused key %u validates as unnamed", k);
    checkf(strcmp(key_name(k), "unknown") == 0, "unused key %u is not named", k);
  }
}

static void check_validation() {
  printf("-- validation --\n");
  const uint32_t good = encode_packet(KEY_BRIGHT_UP, 6);

  check(decode_packet(good).valid(), "a well-formed frame validates");
  check(!decode_packet(good ^ 0x1).check_ok, "a corrupted checksum nibble is rejected");
  check(!decode_packet(good ^ 0x80).check_ok, "a corrupted key bit is rejected");
  check(!decode_packet(good ^ 0x10).check_ok, "a corrupted counter bit is rejected");
  check(!decode_packet(good ^ 0x10000000).prefix_ok, "a wrong prefix is rejected");
  check(!decode_packet(good ^ 0x10000000).valid(), "a wrong prefix fails validation");

  // Every single-bit error anywhere in the word must be caught by one field or
  // the other. A 4-bit XOR checksum cannot catch every multi-bit error, but it
  // has to catch every single-bit one.
  int missed = 0;
  for (int bit = 0; bit < 32; bit++)
    if (decode_packet(good ^ (1u << bit)).valid())
      missed++;
  checkf(missed == 0, "all 32 single-bit corruptions rejected (%d slipped through)", missed);

  {  // format_bits(), which is what the frame dump prints.
    char buf[PACKET_BITS + 1];
    format_bits(0xA1D8218F, buf);
    check(std::string(buf) == "10100001110110000010000110001111", "format_bits is MSB first");
    check(std::string(buf) == to_bits(0xA1D8218F), "format_bits agrees with the test helper");
  }

  // The checksum alone must reject 15 of every 16 random payloads.
  int accepted = 0, total = 0;
  for (uint8_t k = 0; k < 32; k++)
    for (uint8_t c = 0; c < 8; c++)
      for (uint8_t cs = 0; cs < 16; cs++) {
        total++;
        const uint32_t word = (PREFIX << 12) | ((uint32_t) k << 7) | ((uint32_t) c << 4) | cs;
        if (decode_packet(word).valid())
          accepted++;
      }
  checkf(accepted * 16 == total, "checksum accepts 1 payload in 16 (%d of %d)", accepted, total);
}

static void check_press_tracker() {
  printf("-- press / repeat tracking --\n");
  const uint32_t code = encode_packet(KEY_BRIGHT_UP, 2);
  const uint32_t other = encode_packet(KEY_BRIGHT_UP, 3);
  const uint32_t period = 41;  // ms between frames

  {  // A plain tap: five identical frames, one event.
    PressTracker t;
    int emitted = 0;
    for (int f = 0; f < 5; f++)
      if (t.feed(code, f * period).emit)
        emitted++;
    checkf(emitted == 1, "a 5-frame press fires once (fired %d)", emitted);
  }
  {  // The AGC ate frame 1 and only four arrived. Still one event, not zero.
    PressTracker t;
    int emitted = 0;
    for (int f = 0; f < 4; f++)
      if (t.feed(code, f * period).emit)
        emitted++;
    checkf(emitted == 1, "a 4-frame press still fires once (fired %d)", emitted);
  }
  {  // A hold: one press, then one repeat per frame past the fifth, numbered
    // from 1 and never skipping.
    PressTracker t;
    std::vector<uint32_t> events;
    for (int f = 0; f < 12; f++) {
      const PressEvent e = t.feed(code, f * period);
      if (e.emit)
        events.push_back(e.repeat);
    }
    checkf(events.size() == 8, "12 frames give 1 press + 7 repeats (gave %zu)", events.size());
    check(!events.empty() && events[0] == 0, "the first event is the press, repeat 0");
    bool sequential = true;
    for (size_t i = 1; i < events.size(); i++)
      sequential = sequential && events[i] == i;
    check(sequential, "repeats are numbered 1,2,3... with no gaps");
  }
  {  // Two taps of the same button: the counter advances, so the codewords
    // differ and both must fire even back to back.
    PressTracker t;
    int emitted = 0;
    for (int f = 0; f < 5; f++)
      if (t.feed(code, f * period).emit)
        emitted++;
    for (int f = 0; f < 5; f++)
      if (t.feed(other, 300 + f * period).emit)
        emitted++;
    checkf(emitted == 2, "two presses of one button fire twice (fired %d)", emitted);
  }
  {  // The counter wraps at 8, so the ninth press of one button repeats its
    // codeword. Only the run timeout separates it from a hold.
    PressTracker t;
    check(t.feed(code, 0).emit, "first press fires");
    check(!t.feed(code, 40).emit, "its second frame is swallowed");
    check(t.feed(code, 40 + t.get_run_timeout_ms()).emit,
          "the same codeword after the run timeout is a new press");
  }
  {  // A different button mid-run always starts a new run immediately.
    PressTracker t;
    check(t.feed(code, 0).emit, "first press fires");
    const PressEvent e = t.feed(encode_packet(KEY_FAN_1, 3), 41);
    check(e.emit && e.repeat == 0, "a different codeword fires immediately as a press");
  }
  {  // A hold that drops frames must not split into two presses, as long as the
    // gap stays inside the run timeout.
    PressTracker t;
    int presses = 0, repeats = 0;
    uint32_t now = 0;
    for (int f = 0; f < 20; f++) {
      if (f == 7 || f == 8 || f == 9) {  // three frames lost mid-hold
        now += period;
        continue;
      }
      const PressEvent e = t.feed(code, now);
      if (e.emit)
        (e.repeat == 0 ? presses : repeats)++;
      now += period;
    }
    checkf(presses == 1, "a hold with dropped frames stays one press (got %d)", presses);
    checkf(repeats == 12, "and keeps repeating after the dropout (got %d)", repeats);
  }
  {  // press_frames: 1 means report every frame the remote sends.
    PressTracker t;
    t.set_press_frames(1);
    int emitted = 0;
    for (int f = 0; f < 5; f++)
      if (t.feed(code, f * period).emit)
        emitted++;
    checkf(emitted == 5, "press_frames=1 reports every frame (got %d)", emitted);
  }
}

// End to end: synthesised pulses in, press events out, through the same
// walk_frames() the component calls.
static void check_bursts() {
  printf("-- synthesised bursts, end to end --\n");
  const uint8_t key = KEY_TEMP_UP;
  const uint32_t code = encode_packet(key, 4);

  {  // idle: 12ms -- the whole burst arrives as one capture.
    PressTracker t;
    Packet p{};
    FrameWalk walk{};
    const std::vector<uint32_t> events = run_capture(t, make_burst(code, 5), 1000, &p, &walk);
    checkf(walk.decoded == 5, "all 5 frames of a clean burst decode (got %u)",
           (unsigned) walk.decoded);
    checkf(events.size() == 1, "one capture holding a whole press fires once (fired %zu)",
           events.size());
    checkf(p.key == key, "and reports key %u, got %u", key, p.key);
    checkf(p.counter == 4, "and counter 4, got %u", p.counter);
  }
  {  // idle: 4ms -- each frame is its own capture, 41 ms apart. Same answer.
    PressTracker t;
    int emitted = 0;
    for (int f = 0; f < 5; f++) {
      std::vector<int32_t> one;
      append_frame(one, code, f == 0 ? 89 : 22);
      emitted += (int) run_capture(t, one, 1000 + f * 41).size();
    }
    checkf(emitted == 1, "the same press split across 5 captures fires once (fired %d)", emitted);
  }
  {  // The AGC destroyed frame 1. The press must survive on frames 2-5, which
    // is the entire reason this component exists.
    PressTracker t;
    FrameWalk walk{};
    Packet p{};
    const std::vector<uint32_t> events = run_capture(t, make_burst(code, 5, 1), 1000, &p, &walk);
    checkf(walk.decoded == 4, "a damaged frame 1 drops out, 4 remain (got %u)",
           (unsigned) walk.decoded);
    checkf(events.size() == 1, "the press is still reported exactly once (fired %zu)",
           events.size());
    checkf(p.raw == code, "and carries the right codeword");
  }
  {  // A held button: a long burst in one capture at idle: 12ms.
    PressTracker t;
    const std::vector<uint32_t> events = run_capture(t, make_burst(code, 20), 1000);
    checkf(events.size() == 16, "a 20-frame hold gives 1 press + 15 repeats (gave %zu)",
           events.size());
  }
  {  // Idle noise: pulses that are not the remote's symbols must produce
    // nothing at all, not a half-decoded word.
    PressTracker t;
    std::vector<int32_t> noise;
    for (int i = 0; i < 400; i++) {
      noise.push_back(120 + (i * 137) % 900);
      noise.push_back(-(90 + (i * 211) % 800));
    }
    FrameWalk walk{};
    const std::vector<uint32_t> events = run_capture(t, noise, 1000, nullptr, &walk);
    checkf(events.empty(), "noise fires nothing (fired %zu)", events.size());
    checkf(walk.decoded == 0, "and decodes no frames (decoded %u)", (unsigned) walk.decoded);
  }
  {  // Every button, end to end, at a plausible counter.
    for (size_t i = 0; i < KEY_COUNT; i++) {
      PressTracker t;
      Packet p{};
      const uint32_t word = encode_packet(KEY_TABLE[i].key, (uint8_t) (i % 8));
      const std::vector<uint32_t> events = run_capture(t, make_burst(word, 5), 1000, &p);
      checkf(events.size() == 1 && p.key == KEY_TABLE[i].key,
             "burst for %s decodes to one press of key %u", KEY_TABLE[i].name, KEY_TABLE[i].key);
    }
  }
}

// --- transmit path ----------------------------------------------------------

// Everything the builders emit for one press: the preamble once, then `frames`
// copies of the codeword, exactly as the relay stacks them into a transmit call.
static std::vector<int32_t> build_burst(uint8_t key, uint8_t counter, int frames,
                                        bool preamble = true) {
  std::vector<int32_t> out;
  int32_t buffer[FRAME_ENTRIES];
  if (preamble) {
    const size_t n = build_preamble(buffer, FRAME_ENTRIES);
    out.insert(out.end(), buffer, buffer + n);
  }
  for (int f = 0; f < frames; f++) {
    const size_t n = build_frame(key, counter, buffer, FRAME_ENTRIES);
    out.insert(out.end(), buffer, buffer + n);
  }
  return out;
}

static void check_waveform() {
  printf("-- transmit waveform --\n");

  {  // The literal timings, against PROTOCOL.md section 1. A round trip cannot
    // catch a wrong nominal -- the decoder's windows are 50% wide -- so these
    // are asserted as numbers.
    int32_t buffer[FRAME_ENTRIES];
    checkf(build_preamble(buffer, FRAME_ENTRIES) == 2, "the preamble is 2 entries");
    checkf(buffer[0] == 335, "preamble mark is 335us, got %d", buffer[0]);
    checkf(buffer[1] == -7690, "preamble gap is 7690us, got %d", buffer[1]);

    // bright+ at counter 0: 0xA1D8218F, so bit 31 is 1 and bit 30 is 0.
    const size_t n = build_frame(KEY_BRIGHT_UP, 0, buffer, FRAME_ENTRIES);
    checkf(n == 66, "one frame is 66 entries, got %zu", n);
    checkf(buffer[0] == 756 && buffer[1] == -252, "a 1 bit is 756us mark + 252us space, got %d/%d",
           buffer[0], buffer[1]);
    checkf(buffer[2] == 252 && buffer[3] == -756, "a 0 bit is 252us mark + 756us space, got %d/%d",
           buffer[2], buffer[3]);
    checkf(buffer[64] == 330, "the stop mark is 330us, got %d", buffer[64]);
    checkf(buffer[65] == -8787, "the inter-frame gap is 8787us, got %d", buffer[65]);

    // Marks positive, spaces negative, alternating -- the convention
    // decode_frame() reads and remote_transmitter writes.
    bool alternates = true;
    for (size_t i = 0; i < n; i++)
      alternates = alternates && ((i % 2 == 0) ? buffer[i] > 0 : buffer[i] < 0);
    check(alternates, "marks are positive and spaces negative, alternating");

    // Every bit period is the nominal 1008us; only the split moves.
    bool rigid = true;
    for (size_t i = 0; i < PACKET_BITS * 2; i += 2)
      rigid = rigid && (buffer[i] - buffer[i + 1]) == 1008;
    check(rigid, "every bit period is 4 ticks, 1008us");

    checkf(FRAME_PERIOD_US == 41373, "a frame plus its gap is 41373us, got %u",
           (unsigned) FRAME_PERIOD_US);
    checkf(build_preamble(buffer, 1) == 0, "build_preamble refuses a short buffer");
    checkf(build_frame(KEY_BRIGHT_UP, 0, buffer, FRAME_ENTRIES - 1) == 0,
           "build_frame refuses a short buffer");
  }

  {  // Round trip: emitted durations back through the receive path, which is
    // the validated one, so it makes a good oracle for this direction.
    for (size_t i = 0; i < KEY_COUNT; i++) {
      for (uint8_t counter = 0; counter < 8; counter++) {
        const std::vector<int32_t> burst = build_burst(KEY_TABLE[i].key, counter, 5);
        int decoded = 0;
        bool right = true;
        walk_frames(burst.data(), burst.size(), [&](uint32_t bits) {
          const Packet p = decode_packet(bits);
          decoded++;
          right = right && p.valid() && p.key == KEY_TABLE[i].key && p.counter == counter;
        });
        checkf(decoded == 5 && right, "%s at counter %u: 5 frames out, all decoding back",
               KEY_TABLE[i].name, counter);
      }
      // And against the captured codeword itself, not just against the encoder.
      const std::vector<int32_t> burst = build_burst(KEY_TABLE[i].key, 0, 1);
      uint32_t word = 0;
      walk_frames(burst.data(), burst.size(), [&](uint32_t bits) { word = bits; });
      checkf(word == KEY_TABLE[i].cnt0_frame, "%s emits 0x%08X, PROTOCOL.md says 0x%08X",
             KEY_TABLE[i].name, word, KEY_TABLE[i].cnt0_frame);
    }
  }

  {  // The preamble must not be mistaken for a frame, and must not swallow one.
    const std::vector<int32_t> burst = build_burst(KEY_FAN_3, 5, 1);
    FrameWalk walk{};
    PressTracker t;
    const std::vector<uint32_t> events = run_capture(t, burst, 1000, nullptr, &walk);
    // Three chunks: the preamble, the frame, and the empty tail behind the
    // trailing gap. Only the frame is 32 clean bits.
    checkf(walk.chunks == 3, "preamble + frame splits into 3 chunks, got %u",
           (unsigned) walk.chunks);
    checkf(walk.decoded == 1, "and exactly one of them decodes, got %u", (unsigned) walk.decoded);
    checkf(events.size() == 1, "and it reports one press, got %zu", events.size());
  }

  {  // A hold is injected as separate calls: preamble + frame, then bare frames.
    // Concatenated they have to read as one continuous stream of frames.
    std::vector<int32_t> stream = build_burst(KEY_TEMP_DOWN, 2, 1);
    for (int i = 0; i < 6; i++) {
      const std::vector<int32_t> more = build_burst(KEY_TEMP_DOWN, 2, 1, false);
      stream.insert(stream.end(), more.begin(), more.end());
    }
    const FrameWalk walk = walk_frames(stream.data(), stream.size(), [](uint32_t) {});
    checkf(walk.decoded == 7, "an injected hold of 7 frames decodes as 7, got %u",
           (unsigned) walk.decoded);
  }
}

// --- the relay decision -----------------------------------------------------
//
// Driven by synthesised event streams rather than by pulses: the gate sits
// above the frame layer and only ever sees presses, repeats and the clock.

// Everything the gate put on the wire, one entry per injected frame.
struct Injected {
  uint8_t key;
  uint8_t counter;
  bool preamble;  // true on the frame that carried the burst's preamble
};

static void collect(std::vector<Injected> &out, const InjectPlan &plan) {
  for (uint8_t f = 0; f < plan.frames; f++)
    out.push_back(Injected{plan.key, plan.counter, plan.preamble && f == 0});
}

static void check_relay_gate() {
  printf("-- relay decision --\n");
  const uint32_t period = 41;  // ms between received frames
  const uint8_t key = KEY_FAN_2;

  {  // A tap: a press, then silence. Nothing goes out until the deadline, and
    // then exactly one frame.
    RelayGate gate;
    std::vector<Injected> sent;
    collect(sent, gate.press(key, 0));
    checkf(sent.empty(), "nothing is injected on the press event (sent %zu)", sent.size());
    for (uint32_t now = 0; now < gate.get_decision_delay_ms(); now += 10) {
      collect(sent, gate.poll(now));
      checkf(sent.empty(), "still nothing at %u ms, before the deadline", (unsigned) now);
    }
    collect(sent, gate.poll(gate.get_decision_delay_ms()));
    checkf(sent.size() == 1, "the deadline injects exactly one frame (sent %zu)", sent.size());
    check(!sent.empty() && sent[0].preamble, "and it carries the preamble");
    check(!sent.empty() && sent[0].key == key, "and the key that was pressed");

    // The deadline fires once, not on every subsequent poll.
    collect(sent, gate.poll(gate.get_decision_delay_ms() + 1000));
    checkf(sent.size() == 1, "and does not fire again (sent %zu)", sent.size());
  }

  {  // A hold of N repeats injects N frames, starting at the first repeat --
    // which arrives well before the deadline would have.
    const int repeats = 12;
    RelayGate gate;
    std::vector<Injected> sent;
    uint32_t now = 0;
    collect(sent, gate.press(key, now));
    for (int r = 1; r <= repeats; r++) {
      now += period;
      collect(sent, gate.poll(now));
      collect(sent, gate.repeat(key));
    }
    checkf((int) sent.size() == repeats, "a hold of %d repeats injects %d frames (sent %zu)",
           repeats, repeats, sent.size());
    check(!sent.empty() && sent[0].preamble, "the first injected frame carries the preamble");
    bool once = true;
    for (size_t i = 1; i < sent.size(); i++)
      once = once && !sent[i].preamble;
    check(once, "and no later frame repeats it");

    // Every frame of the hold is the same press: one counter throughout.
    bool same = true;
    for (const Injected &frame : sent)
      same = same && frame.counter == sent[0].counter && frame.key == key;
    check(same, "every frame of a hold carries one key and one counter");
  }

  {  // The hold stops when the button is released: no repeats, no frames, and
    // the deadline has long since been resolved.
    RelayGate gate;
    std::vector<Injected> sent;
    collect(sent, gate.press(key, 0));
    collect(sent, gate.repeat(key));
    const size_t during = sent.size();
    for (uint32_t now = 2 * period; now < 5000; now += period)
      collect(sent, gate.poll(now));
    checkf(sent.size() == during, "a released hold injects nothing further (sent %zu more)",
           sent.size() - during);
  }

  {  // Consecutive presses must differ, or the MCU cannot tell them apart.
    RelayGate gate;
    std::vector<Injected> sent;
    uint32_t now = 0;
    for (int press = 0; press < 16; press++) {
      collect(sent, gate.press(key, now));
      now += gate.get_decision_delay_ms() + 1;
      collect(sent, gate.poll(now));
      now += 1000;
    }
    checkf(sent.size() == 16, "16 taps inject 16 frames (sent %zu)", sent.size());
    bool differ = true;
    for (size_t i = 1; i < sent.size(); i++)
      differ = differ && sent[i].counter != sent[i - 1].counter;
    check(differ, "consecutive presses carry different counters");
    // 3 bits, so it has to wrap rather than saturate.
    bool wrapped = false;
    for (const Injected &frame : sent)
      wrapped = wrapped || frame.counter < sent[0].counter;
    check(wrapped, "and the counter wraps at 8 rather than sticking");
  }

  {  // A press whose sixth frame was lost: the deadline fires and calls it a
    // tap, then the seventh frame arrives. The stream has to pick up, because
    // the tap already carried this run's counter.
    RelayGate gate;
    std::vector<Injected> sent;
    collect(sent, gate.press(key, 0));
    collect(sent, gate.poll(gate.get_decision_delay_ms()));
    checkf(sent.size() == 1, "the deadline injected the tap (sent %zu)", sent.size());
    collect(sent, gate.repeat(key));
    collect(sent, gate.repeat(key));
    checkf(sent.size() == 3, "and the late repeats extend it (sent %zu)", sent.size());
    bool same = true;
    for (const Injected &frame : sent)
      same = same && frame.counter == sent[0].counter;
    check(same, "on the counter the tap already used, so the MCU sees one press");
  }

  {  // Two buttons inside one decision window. The first resolves as the tap it
    // was; a repeat of it can no longer arrive.
    RelayGate gate;
    std::vector<Injected> sent;
    collect(sent, gate.press(KEY_FAN_1, 0));
    collect(sent, gate.press(KEY_FAN_OFF, 50));
    checkf(sent.size() == 1 && sent[0].key == KEY_FAN_1,
           "a second press flushes the first as a tap (sent %zu)", sent.size());
    collect(sent, gate.poll(50 + gate.get_decision_delay_ms()));
    checkf(sent.size() == 2 && sent[1].key == KEY_FAN_OFF, "and the second follows on its own deadline");
    checkf(sent[0].counter != sent[1].counter, "with distinct counters");
    // A stale repeat of the superseded run must not reopen it.
    const size_t before = sent.size();
    collect(sent, gate.repeat(KEY_FAN_1));
    checkf(sent.size() == before, "a repeat of the superseded press injects nothing");
  }

  {  // tap_frames is the knob for "one frame may not be enough". Every frame of
    // the tap is still one press.
    RelayGate gate;
    gate.set_tap_frames(5);
    std::vector<Injected> sent;
    collect(sent, gate.press(key, 0));
    collect(sent, gate.poll(gate.get_decision_delay_ms()));
    checkf(sent.size() == 5, "tap_frames=5 injects 5 frames (sent %zu)", sent.size());
    bool same = true;
    for (const Injected &frame : sent)
      same = same && frame.counter == sent[0].counter;
    check(same, "all on one counter");
    checkf(sent[0].preamble && !sent[1].preamble, "with the preamble once, at the front");
  }

  {  // send_key: one press, its own counter, and it does not disturb a relay
    // decision already in flight.
    RelayGate gate;
    std::vector<Injected> sent;
    collect(sent, gate.press(key, 0));
    collect(sent, gate.originate(KEY_LIGHT_TOGGLE));
    checkf(sent.size() == 1 && sent[0].key == KEY_LIGHT_TOGGLE,
           "send_key injects one frame immediately (sent %zu)", sent.size());
    check(sent[0].preamble, "with a preamble of its own");
    collect(sent, gate.poll(gate.get_decision_delay_ms()));
    checkf(sent.size() == 2 && sent[1].key == key, "and the pending press still resolves");
    checkf(sent[0].counter != sent[1].counter, "on a different counter");
  }
}

static void check_key_domains() {
  printf("-- fan / light classification --\n");

  // Transcribed from the button names in PROTOCOL.md section 4. The five marked
  // UNVERIFIED could act on the fan, the light or both; nobody has watched.
  struct DomainRow {
    uint8_t key;
    KeyDomain domain;
  };
  static const DomainRow ROWS[] = {
      {KEY_BRIGHT_UP, DOMAIN_LIGHT},          {KEY_BRIGHT_DOWN, DOMAIN_LIGHT},
      {KEY_TEMP_UP, DOMAIN_LIGHT},            {KEY_TEMP_DOWN, DOMAIN_LIGHT},
      {KEY_LIGHT_TOGGLE, DOMAIN_LIGHT},       {KEY_CYCLE_FULL_BRIGHT, DOMAIN_LIGHT},
      {KEY_FAN_1, DOMAIN_FAN},                {KEY_FAN_2, DOMAIN_FAN},
      {KEY_FAN_3, DOMAIN_FAN},                {KEY_FAN_4, DOMAIN_FAN},
      {KEY_FAN_5, DOMAIN_FAN},                {KEY_FAN_6, DOMAIN_FAN},
      {KEY_FAN_OFF, DOMAIN_FAN},              {KEY_FAN_FORWARD, DOMAIN_FAN},
      {KEY_FAN_REVERSE, DOMAIN_FAN},          {KEY_ALL_OFF, DOMAIN_UNVERIFIED},
      {KEY_NIGHT_MODE, DOMAIN_UNVERIFIED},    {KEY_NATURAL_WIND, DOMAIN_UNVERIFIED},
      {KEY_TIMER_2H, DOMAIN_UNVERIFIED},      {KEY_TIMER_4H, DOMAIN_UNVERIFIED},
  };
  const size_t rows = sizeof(ROWS) / sizeof(ROWS[0]);
  checkf(rows == KEY_COUNT, "every button is classified (%zu of %zu)", rows, KEY_COUNT);

  for (size_t i = 0; i < rows; i++) {
    checkf(key_domain(ROWS[i].key) == ROWS[i].domain, "%s is classified as expected",
           key_name(ROWS[i].key));
    check(relay_key(RELAY_ALL, ROWS[i].key), "relay: all forwards everything");
    check(!relay_key(RELAY_NONE, ROWS[i].key), "relay: none forwards nothing");
    // Fan mode drops the light keys and keeps the unverified ones: dropping a
    // button whose effect is unknown would break something that works.
    checkf(relay_key(RELAY_FAN, ROWS[i].key) == (ROWS[i].domain != DOMAIN_LIGHT),
           "relay: fan handles %s correctly", key_name(ROWS[i].key));
  }

  // A key this remote does not have is unverified, never silently a fan key.
  check(key_domain(0) == DOMAIN_UNVERIFIED, "an unlisted key is unverified");
}

// --- log replay (diagnostic) ------------------------------------------------

struct Capture {
  std::string timestamp;
  std::vector<int32_t> durations;
};

// The `raw` dumper flushes at a 256-char line buffer and continues on the next
// line with the same tag but no "Received Raw:" prefix. One log line is not one
// capture; continuation lines must be appended to the capture in progress.
static std::vector<Capture> parse_captures(const std::string &text) {
  std::vector<Capture> captures;
  bool have_current = false;
  Capture current;

  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    size_t pos = line.find("remote.raw:");
    if (pos == std::string::npos)
      continue;
    pos = line.find("]: ", pos);
    if (pos == std::string::npos)
      continue;
    std::string body = line.substr(pos + 3);

    const std::string prefix = "Received Raw:";
    if (body.compare(0, prefix.size(), prefix) == 0) {
      if (have_current)
        captures.push_back(current);
      current = Capture();
      // "[HH:MM:SS.mmm][I][remote.raw:026]: ..." -- 12 chars after the bracket.
      current.timestamp = line.size() > 13 ? line.substr(1, 12) : std::string();
      have_current = true;
      body = body.substr(prefix.size());
    }
    if (!have_current)
      continue;

    for (size_t i = 0; i < body.size();) {
      if (body[i] == '-' || (body[i] >= '0' && body[i] <= '9')) {
        char *end = nullptr;
        long value = strtol(body.c_str() + i, &end, 10);
        current.durations.push_back((int32_t) value);
        i = (size_t) (end - body.c_str());
      } else {
        i++;
      }
    }
  }
  if (have_current)
    captures.push_back(current);
  return captures;
}

// "HH:MM:SS.mmm" -> milliseconds since midnight. Good enough to drive the press
// tracker; a session straddling midnight would wrap.
static uint32_t timestamp_ms(const std::string &timestamp) {
  if (timestamp.size() < 12)
    return 0;
  const uint32_t hours = (uint32_t) atoi(timestamp.substr(0, 2).c_str());
  const uint32_t minutes = (uint32_t) atoi(timestamp.substr(3, 2).c_str());
  const uint32_t seconds = (uint32_t) atoi(timestamp.substr(6, 2).c_str());
  const uint32_t millis = (uint32_t) atoi(timestamp.substr(9, 3).c_str());
  return ((hours * 60 + minutes) * 60 + seconds) * 1000 + millis;
}

static bool read_file(const std::string &path, std::string *out) {
  std::ifstream file(path);
  if (!file) {
    fprintf(stderr, "%s: cannot open\n", path.c_str());
    return false;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  *out = buffer.str();
  return true;
}

// Exactly what `dump_frames: true` prints, through the same walk_frames() the
// component calls: one line per capture, comma-separated codewords.
static void dump_capture(const Capture &capture) {
  std::vector<uint32_t> found;
  const FrameWalk walk = walk_frames(capture.durations.data(), capture.durations.size(),
                                     [&](uint32_t bits) { found.push_back(bits); });
  if (found.empty())
    return;  // silent, the same as the component

  std::string words;
  for (size_t i = 0; i < found.size(); i++) {
    if (i != 0)
      words += ", ";
    words += to_bits(found[i]);
  }
  printf("[%s][D][fan_rf]: %u symbols, %u chunk%s, %u frame%s: %s\n", capture.timestamp.c_str(),
         (unsigned) capture.durations.size(), (unsigned) walk.chunks,
         walk.chunks == 1 ? "" : "s", (unsigned) walk.decoded, walk.decoded == 1 ? "" : "s",
         words.c_str());
}

static int replay(const std::vector<std::string> &paths) {
  PressTracker tracker;
  int presses = 0, repeats = 0, frames = 0, rejected = 0;
  std::map<uint8_t, int> keys;

  for (const std::string &path : paths) {
    std::string text;
    if (!read_file(path, &text))
      return 1;
    const std::vector<Capture> captures = parse_captures(text);
    if (captures.empty()) {
      fprintf(stderr, "%s: no 'Received Raw:' lines found\n", path.c_str());
      continue;
    }

    printf("\n=== %s: %zu captures ===\n", path.c_str(), captures.size());
    for (const Capture &capture : captures) {
      const uint32_t now = timestamp_ms(capture.timestamp);
      walk_frames(capture.durations.data(), capture.durations.size(), [&](uint32_t bits) {
        frames++;
        const Packet p = decode_packet(bits);
        if (!p.valid()) {
          rejected++;
          printf("%-13s %s  REJECTED (%s%s)\n", capture.timestamp.c_str(), to_bits(bits).c_str(),
                 p.prefix_ok ? "" : "bad prefix ", p.check_ok ? "" : "bad checksum");
          return;
        }
        const PressEvent e = tracker.feed(p.raw, now);
        if (!e.emit)
          return;
        keys[p.key]++;
        (e.repeat == 0 ? presses : repeats)++;
        printf("%-13s %s  %-6s key=%2u (%s) counter=%u\n", capture.timestamp.c_str(),
               to_bits(bits).c_str(), e.repeat == 0 ? "PRESS" : "repeat", p.key, key_name(p.key),
               p.counter);
      });
    }
  }

  printf("\n=== summary ===\n");
  printf("frames decoded: %d (%d rejected by prefix/checksum)\n", frames, rejected);
  printf("presses: %d, held repeats: %d\n", presses, repeats);
  printf("keys seen:");
  for (const auto &entry : keys)
    printf(" %u(%s)=%d", entry.first, key_name(entry.first), entry.second);
  printf("\n");
  return presses > 0 ? 0 : 1;
}

int main(int argc, char **argv) {
  std::vector<std::string> paths;
  bool dump_mode = false;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--dump") == 0)
      dump_mode = true;
    else if (argv[i][0] == '-') {
      fprintf(stderr, "usage: fan_rf_selftest [--dump] [LOG...]\n");
      return 2;
    } else
      paths.push_back(argv[i]);
  }

  if (dump_mode) {
    if (paths.empty()) {
      fprintf(stderr, "--dump needs a log file\n");
      return 2;
    }
    for (const std::string &path : paths) {
      std::string text;
      if (!read_file(path, &text))
        return 1;
      for (const Capture &capture : parse_captures(text))
        dump_capture(capture);
    }
    return 0;
  }

  check_key_table();
  check_validation();
  check_press_tracker();
  check_bursts();
  check_waveform();
  check_relay_gate();
  check_key_domains();
  printf("\n%d checks, %d failures\n", checks, failures);

  if (!paths.empty() && failures == 0)
    return replay(paths);
  return failures == 0 ? 0 : 1;
}
