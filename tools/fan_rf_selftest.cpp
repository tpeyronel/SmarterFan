// Host self-test for the on-device decoder.
//
// Compiles firmware/components/fan_rf/fan_rf_protocol.h -- the exact header the
// firmware uses -- against the captured `dump: raw` logs, so the decoder can be
// validated before anything is flashed.
//
//   c++ -std=c++17 -O2 -o /tmp/fan_rf_selftest tools/fan_rf_selftest.cpp
//   /tmp/fan_rf_selftest log3.txt          # expect 37/37
//
// Output deliberately mirrors tools/decode_fan_rf.py so the two can be diffed
// against each other. With --frames it also reports the clean-frame rate by
// position within the burst, which is the measurement that motivated the whole
// component.

#include "../firmware/components/fan_rf/fan_rf_protocol.h"
#include "../firmware/components/fan_rf_dump/fan_rf_dump_protocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace smarterfan::fan_rf;

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
    const std::string tag = "remote.raw:";
    size_t pos = line.find(tag);
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

// "HH:MM:SS.mmm" -> milliseconds since midnight. Good enough to drive the
// dedup filter; a session that straddles midnight would wrap, and none do.
static uint32_t timestamp_ms(const std::string &timestamp) {
  if (timestamp.size() < 12)
    return 0;
  const uint32_t hours = (uint32_t) atoi(timestamp.substr(0, 2).c_str());
  const uint32_t minutes = (uint32_t) atoi(timestamp.substr(3, 2).c_str());
  const uint32_t seconds = (uint32_t) atoi(timestamp.substr(6, 2).c_str());
  const uint32_t millis = (uint32_t) atoi(timestamp.substr(9, 3).c_str());
  return ((hours * 60 + minutes) * 60 + seconds) * 1000 + millis;
}

static const char *command_name(uint8_t command) {
  switch (command) {
    case 0x21:
      return "brightness+";
    case 0x23:
      return "colour temp+";
    case 0x28:
      return "fan speed 1";
    default:
      return "UNKNOWN";
  }
}

static std::string to_bits(uint32_t value) {
  std::string out(32, '0');
  for (int i = 0; i < 32; i++)
    out[i] = (value & (1u << (31 - i))) ? '1' : '0';
  return out;
}

// Exactly what firmware/components/fan_rf_dump prints, driven through that
// component's OWN walk_frames() -- it carries its own standalone copy of the
// decoder and shares nothing with fan_rf, so testing fan_rf's copy here would
// prove nothing about it. Lets the diagnostic output be read off the captured
// logs without flashing anything.
static void dump_capture(const Capture &capture) {
  namespace dump = smarterfan::fan_rf_dump;
  std::vector<std::pair<uint16_t, uint32_t>> found;
  const dump::FrameWalk walk = dump::walk_frames(
      capture.durations.data(), capture.durations.size(),
      [&](uint16_t position, uint32_t bits) { found.push_back({position, bits}); });

  if (found.empty())
    return;  // silent, the same as the component

  printf("[%s][I][fan_rf_dump]: %u symbols, %u chunks, %u frames decoded\n",
         capture.timestamp.c_str(), (unsigned) capture.durations.size(), (unsigned) walk.chunks,
         (unsigned) walk.decoded);
  for (const auto &entry : found)
    printf("[%s][I][fan_rf_dump]:   f%u %s\n", capture.timestamp.c_str(), (unsigned) entry.first,
           to_bits(entry.second).c_str());
}

// Clean-frame rate by position within a burst: the table from the task notes.
// Only bursts that look like a whole transmission (a preamble plus repeats) are
// counted, so partial captures do not skew the positions.
struct FrameStats {
  int clean[8] = {0};
  int total[8] = {0};
};

static void tally_frames(const std::vector<int32_t> &durations, FrameStats *stats) {
  std::vector<std::pair<size_t, size_t>> chunks;
  size_t start = 0;
  for (size_t i = 0; i <= durations.size(); i++) {
    if (i == durations.size() || is_frame_gap(durations[i])) {
      chunks.push_back({start, i});
      start = i + 1;
    }
  }
  // Drop a leading preamble chunk (a lone mark) so position 1 is the first
  // data frame, matching how the burst is described.
  size_t first = 0;
  if (!chunks.empty() && chunks[0].second - chunks[0].first < PACKET_BITS * 2)
    first = 1;
  for (size_t c = first; c < chunks.size() && c - first < 8; c++) {
    const size_t len = chunks[c].second - chunks[c].first;
    if (len < PACKET_BITS * 2)
      continue;  // truncated tail, not a frame that had a chance
    uint32_t bits;
    stats->total[c - first]++;
    if (decode_frame(&durations[chunks[c].first], len, &bits))
      stats->clean[c - first]++;
  }
}


// --- unit checks -----------------------------------------------------------
//
// The log corpus happens to contain no burst that split across two captures,
// so replaying it exercises the decoder but never the dedupe. These cases pin
// down PressFilter's contract directly.

static int failures = 0;

static void check(bool condition, const char *what) {
  if (!condition) {
    printf("UNIT FAIL: %s\n", what);
    failures++;
  }
}

static void run_unit_checks() {
  {  // A press is one event; the same codeword arriving again is the same press.
    PressFilter filter;
    filter.set_window_ms(1000);
    check(filter.accept(0xA1D82169, 0), "first press accepted");
    check(!filter.accept(0xA1D82169, 40), "same code 40ms later suppressed");
    check(!filter.accept(0xA1D82169, 250), "same code 250ms later suppressed");
  }
  {  // A held button keeps refreshing the window instead of leaking one event
    // per window's worth of repeats.
    PressFilter filter;
    filter.set_window_ms(1000);
    check(filter.accept(0xA1D82169, 0), "first press accepted");
    for (uint32_t t = 200; t <= 5000; t += 200)
      check(!filter.accept(0xA1D82169, t), "held button stays suppressed");
    check(filter.accept(0xA1D82169, 6200), "same code after a real gap accepted");
  }
  {  // Consecutive presses differ in the counter nibble, so back-to-back
    // presses of the SAME button must both fire.
    PressFilter filter;
    filter.set_window_ms(1000);
    check(filter.accept(0xA1D82169, 0), "brightness+ counter 6 accepted");
    check(filter.accept(0xA1D8217A, 240), "brightness+ counter 7 accepted 240ms later");
    check(filter.accept(0xA1D8218E, 480), "brightness+ counter 0 accepted 480ms later");
  }
  {  // The packet field split, against the worked example from the protocol
    // notes: brightness+ with counter 6 is A1 D8 21 E9.
    Packet p = decode_packet(0xA1D821E9);
    check(p.device == 0xA1D8, "device id");
    check(p.command == 0x21, "command byte");
    check(p.counter == 6, "press counter");
    check(p.domain == 1, "light domain flag");
    check(p.check_ok && p.device_ok && p.domain_ok, "example packet validates");
    check(!decode_packet(0xA1D821E8).check_ok, "a corrupted check nibble is rejected");
    check(!decode_packet(0xA1D921E9).device_ok, "a wrong device id is rejected");
    check(decode_packet(0xA1D82868).valid(), "fan command validates");
    check(decode_packet(0xA1D82868).domain == 0, "fan domain flag");
  }
  {  // An unknown command byte is valid-but-unnamed, not an error: only three
    // of the remote's buttons have ever been captured.
    const uint8_t command = 0x25, high = 0xE;
    const uint32_t code = ((uint32_t) DEVICE_ID << 16) | ((uint32_t) command << 8) |
                          (high << 4) | (uint8_t) (high ^ (command & 0x0F) ^ CHECK_XOR);
    check(decode_packet(code).valid(), "unknown command with a good checksum validates");
  }
}

int main(int argc, char **argv) {
  std::vector<std::string> paths;
  bool show_frames = false;
  bool dump_mode = false;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--frames") == 0)
      show_frames = true;
    else if (strcmp(argv[i], "--dump") == 0)
      dump_mode = true;
    else
      paths.push_back(argv[i]);
  }
  if (paths.empty()) {
    fprintf(stderr, "usage: fan_rf_selftest [--frames] [--dump] LOG [LOG...]\n");
    return 2;
  }

  if (dump_mode) {
    for (const std::string &path : paths) {
      std::ifstream file(path);
      if (!file) {
        fprintf(stderr, "%s: cannot open\n", path.c_str());
        return 1;
      }
      std::stringstream buffer;
      buffer << file.rdbuf();
      for (const Capture &capture : parse_captures(buffer.str()))
        dump_capture(capture);
    }
    return 0;
  }

  run_unit_checks();

  int presses = 0, field_pass = 0, rescued = 0, suppressed = 0;
  PressFilter filter;
  std::map<uint8_t, int> commands;
  std::map<int, int> steps;
  int previous_counter = -1;
  FrameStats stats;

  for (const std::string &path : paths) {
    std::ifstream file(path);
    if (!file) {
      fprintf(stderr, "%s: cannot open\n", path.c_str());
      return 1;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::vector<Capture> captures = parse_captures(buffer.str());
    if (captures.empty()) {
      fprintf(stderr, "%s: no 'Received Raw:' lines found\n", path.c_str());
      continue;
    }

    printf("\n=== %s: %zu captures ===\n", path.c_str(), captures.size());
    printf("%-13s %6s %3s  %-34s %-14s %3s %3s\n", "time", "frames", "ok", "code", "cmd", "ctr",
           "chk");

    for (const Capture &capture : captures) {
      tally_frames(capture.durations, &stats);
      DecodeResult r = decode_capture(capture.durations.data(), capture.durations.size());
      if (r.frames_decoded == 0) {
        printf("%-13s %6u %3d  %-34s\n", capture.timestamp.c_str(),
               (unsigned) (r.frames_total ? r.frames_total - 1 : 0), 0, "-- no clean frame --");
        continue;
      }
      const Packet &p = r.packet;
      std::string flags;
      if (!p.device_ok)
        flags += " BAD-ID";
      if (!p.domain_ok)
        flags += " BAD-DOMAIN";
      if (!p.check_ok)
        flags += " BAD-CHECK";
      printf("%-13s %6u %3u  %-34s 0x%02X %-9s %3u %3u%s\n", capture.timestamp.c_str(),
             (unsigned) (r.frames_total ? r.frames_total - 1 : 0), (unsigned) r.votes,
             to_bits(p.raw).c_str(), p.command, command_name(p.command), p.counter, p.check,
             flags.c_str());

      // Exercise the same dedupe the component runs, driven by the log's own
      // timestamps instead of millis().
      if (!filter.accept(p.raw, timestamp_ms(capture.timestamp))) {
        suppressed++;
        continue;
      }

      presses++;
      commands[p.command]++;
      if (p.device_ok && p.check_ok && p.domain_ok)
        field_pass++;
      // A burst where the winner did not come from every frame is one the
      // single-shot rc_switch decoder would have had to get lucky on.
      if (r.votes < r.frames_decoded || r.frames_decoded < 5)
        rescued++;
      if (previous_counter >= 0)
        steps[((int) p.counter - previous_counter + 8) % 8]++;
      previous_counter = p.counter;
    }
  }

  if (presses == 0) {
    fprintf(stderr, "no presses decoded\n");
    return 1;
  }

  printf("\n=== summary: %d presses ===\n", presses);
  printf("commands seen:");
  for (const auto &entry : commands)
    printf(" 0x%02X=%d", entry.first, entry.second);
  printf("\nfield checks: %d/%d pass\n", field_pass, presses);
  printf("counter increments between consecutive presses:");
  for (const auto &entry : steps)
    printf(" %d=%d", entry.first, entry.second);
  printf("\nbursts decoded from fewer than 5 agreeing frames: %d\n", rescued);
  printf("repeat captures suppressed by the %u ms dedup window: %d\n",
         (unsigned) filter.get_window_ms(), suppressed);

  if (show_frames) {
    printf("\nclean-frame rate by position in burst:\n");
    for (int i = 0; i < 8; i++) {
      if (stats.total[i] == 0)
        continue;
      printf("  f%d  %3d/%-3d  %3.0f%%\n", i + 1, stats.clean[i], stats.total[i],
             100.0 * stats.clean[i] / stats.total[i]);
    }
  }

  printf("unit checks: %s\n", failures == 0 ? "pass" : "FAILED");
  return (field_pass == presses && failures == 0) ? 0 : 1;
}
