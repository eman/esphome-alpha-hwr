#include <iostream>
#include <vector>
#include <cstdint>
#include <string>
#include <algorithm>
#include <functional>
#include "fixture_crc.h"
#include "../components/alpha_hwr/transport.h"
#include "../components/alpha_hwr/frame_builder.h"

uint32_t mock_millis = 0;
int tests_passed = 0;
int tests_failed = 0;

#define TEST_ASSERT(condition, message) \
  if (condition) { \
    tests_passed++; \
    std::cout << "[PASS] " << message << std::endl; \
  } else { \
    tests_failed++; \
    std::cout << "[FAIL] " << message << std::endl; \
  }

// The two branches production takes when a chunk cannot be handed to the BLE
// stack. Every write callback in this suite used to return true unconditionally,
// so neither had ever executed: a GATT failure -- the ordinary consequence of a
// link dropping mid-write -- was the one transport path with no coverage at all.
//
// The unwired-writer branch is testable but unreachable in production:
// set_write_callback() is called unconditionally in setup() and loop() only runs
// afterwards, and an unattached characteristic returns false from INSIDE the
// callback, which is the other branch. Covered because a branch that exists
// should behave, not because a node can reach it.
void test_send_failure_fails_the_command_and_frees_the_transport() {
  std::cout << "\n=== Testing send failure (GATT write returns false) ===" << std::endl;
  esphome::alpha_hwr::core::Transport transport;

  bool writes_succeed = false;
  int chunks = 0;
  std::vector<uint8_t> first_bytes;  // which command's bytes actually went out
  transport.set_write_callback([&](const uint8_t *data, size_t len) -> bool {
    chunks++;
    if (len > 0) first_bytes.push_back(data[0]);
    return writes_succeed;
  });

  int cb_calls = 0;
  bool cb_success = true;
  transport.send_command(std::vector<uint8_t>(10, 0xAA), 0, 0,
                         [&](bool ok, const uint8_t *, size_t) { cb_calls++; cb_success = ok; });

  mock_millis += 50;
  transport.loop();

  TEST_ASSERT(chunks == 1, "the write was attempted once");
  TEST_ASSERT(cb_calls == 1, "the command's callback fired exactly once");
  TEST_ASSERT(!cb_success, "and reported failure, not silence");

  // The queue must advance: if the failed command were left at the head, the
  // link would be wedged for every later command -- far worse than the one
  // dropped write. (Leaving state_ in SENDING_CHUNKS is NOT asserted here and
  // deliberately so: a fresh front command re-enters with bytes_sent == 0 and
  // the SENDING_CHUNKS arm repeats the same pacing check, so it is an
  // equivalent mutant that no assertion could distinguish.)
  //
  // Assert on WHICH command's bytes went out, not on how many writes happened.
  // Counting writes cannot tell the second command being sent from the first
  // being re-sent, so a transport that never pops still satisfies a count --
  // the assertion reads as corroboration while discriminating nothing. (That
  // was this test's first form, and a review pass proved it passed against a
  // deliberately non-advancing queue.) The payload bytes differ by design.
  writes_succeed = true;
  transport.send_command(std::vector<uint8_t>(10, 0xBB), 0, 0, nullptr);
  mock_millis += 50;
  transport.loop();
  TEST_ASSERT(first_bytes.size() == 2, "a second write was attempted");
  TEST_ASSERT(first_bytes.size() == 2 && first_bytes[0] == 0xAA && first_bytes[1] == 0xBB,
              "and it carried the SECOND command's bytes, so the queue advanced");
}

void test_send_failure_midway_through_a_chunked_packet() {
  std::cout << "\n=== Testing send failure part-way through a packet ===" << std::endl;
  esphome::alpha_hwr::core::Transport transport;

  int chunks = 0;
  transport.set_write_callback([&](const uint8_t *, size_t) -> bool {
    chunks++;
    return chunks < 2;  // first chunk lands, second fails
  });

  int cb_calls = 0;
  bool cb_success = true;
  // 53 bytes chunks into 20/20/13.
  transport.send_command(std::vector<uint8_t>(53, 0xCC), 0, 0,
                         [&](bool ok, const uint8_t *, size_t) { cb_calls++; cb_success = ok; });

  for (int i = 0; i < 4; i++) { mock_millis += 51; transport.loop(); }

  TEST_ASSERT(chunks == 2, "the third chunk is not attempted after the second fails");
  TEST_ASSERT(cb_calls == 1 && !cb_success,
              "a half-written packet fails the command once, rather than hanging");
}

// A failure PART-WAY through a packet is not the same event as a failure on the
// first chunk, and until now the code could not tell them apart.
//
// If chunk 1 of 3 reached the pump and chunk 2 did not, the pump is holding the
// head of a frame whose length byte promises more. Our own reassembler -- the
// mirror of the pump's -- ignores a frame start while it is mid-frame and only
// abandons a partial after REASSEMBLY_TIMEOUT_MS, so a peer built the same way
// appends the next command to the wreckage and swallows it whole. The caller of
// THAT command then waits out its full timeout with nothing to show.
//
// So after a partial write the transport owes the peer silence long enough for
// its staleness guard to fire, and owes it only then: a first-chunk failure put
// no bytes on the wire and must not pay the stall.
void test_partial_write_holds_off_so_the_peer_can_resync() {
  std::cout << "\n=== Testing hold-off after a partial write ===" << std::endl;
  esphome::alpha_hwr::core::Transport transport;

  int chunks = 0;
  std::vector<uint8_t> first_bytes;
  transport.set_write_callback([&](const uint8_t *data, size_t len) -> bool {
    chunks++;
    if (len > 0) first_bytes.push_back(data[0]);
    return chunks != 2;  // chunk 1 lands, chunk 2 fails
  });

  transport.send_command(std::vector<uint8_t>(53, 0xAA), 0, 0, nullptr);
  transport.send_command(std::vector<uint8_t>(10, 0xBB), 0, 0, nullptr);

  // Drive to the failure: chunk 1 lands on the first loop, chunk 2 fails on the
  // second. Stop there -- a third loop is exactly what must not send anything.
  for (int i = 0; i < 2; i++) { mock_millis += 51; transport.loop(); }
  TEST_ASSERT(chunks == 2, "the second chunk failed");

  // Probe just BELOW the peer's staleness boundary, not at some comfortable
  // fraction of it. A peer built like this one starts its reassembly clock when
  // the first fragment lands and abandons at > REASSEMBLY_TIMEOUT_MS; the
  // failing attempt is at least one pacing interval after that fragment, so the
  // partial is stale at 1000 - 50 = 950 ms after the failure. Anything shorter
  // than that leaves the hold in place and useless, and a probe at 600 ms would
  // wave it through: a 700 ms window passes every assertion in this file if the
  // probe is loose, which is the likeliest way for someone to "trim the stall".
  // Stepped 1 ms at a time so the probe lands exactly on the boundary rather
  // than straddling it: at T+950 the peer's clock reads 1000 since its first
  // fragment, and its guard is strictly `> 1000`, so it is NOT yet stale.
  const uint32_t peer_stale_boundary_ms = 950;
  for (uint32_t t = 0; t < peer_stale_boundary_ms; t++) {
    mock_millis += 1;
    transport.loop();
  }
  bool sent_during_hold = false;
  for (uint8_t b : first_bytes) if (b == 0xBB) sent_during_hold = true;
  TEST_ASSERT(!sent_during_hold,
              "the next command is withheld until the peer's partial must have gone stale");

  // Past it, the link resumes -- the hold is a pause, not a wedge.
  for (uint32_t t = 0; t < 400; t++) { mock_millis += 1; transport.loop(); }
  for (uint8_t b : first_bytes) if (b == 0xBB) sent_during_hold = true;
  TEST_ASSERT(sent_during_hold, "and goes out once the peer's partial frame must have gone stale");
}

void test_first_chunk_failure_does_not_hold_off() {
  std::cout << "\n=== Testing no hold-off when nothing reached the peer ===" << std::endl;
  esphome::alpha_hwr::core::Transport transport;

  int chunks = 0;
  std::vector<uint8_t> first_bytes;
  transport.set_write_callback([&](const uint8_t *data, size_t len) -> bool {
    chunks++;
    if (len > 0) first_bytes.push_back(data[0]);
    return chunks != 1;  // the very first chunk fails; nothing is on the wire
  });

  transport.send_command(std::vector<uint8_t>(10, 0xAA), 0, 0, nullptr);
  transport.send_command(std::vector<uint8_t>(10, 0xBB), 0, 0, nullptr);

  for (int i = 0; i < 4; i++) { mock_millis += 51; transport.loop(); }

  bool sent_second = false;
  for (uint8_t b : first_bytes) if (b == 0xBB) sent_second = true;
  TEST_ASSERT(sent_second,
              "no bytes reached the peer, so the next command is not delayed a second for nothing");
}

void test_missing_write_callback_drops_the_command() {
  std::cout << "\n=== Testing no write callback configured ===" << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  // Deliberately never set one: this is the state between construction and
  // BLEClient wiring up the characteristic.

  int cb_calls = 0;
  bool cb_success = true;
  transport.send_command(std::vector<uint8_t>(10, 0xDD), 0, 0,
                         [&](bool ok, const uint8_t *, size_t) { cb_calls++; cb_success = ok; });

  mock_millis += 50;
  transport.loop();

  TEST_ASSERT(cb_calls == 1 && !cb_success,
              "the command is failed rather than queued forever against a null writer");
}

void test_transport_chunking() {
  std::cout << "\n=== Testing Transport BLE Chunking ===" << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  
  std::vector<std::vector<uint8_t>> sent_chunks;
  transport.set_write_callback([&sent_chunks](const uint8_t* data, size_t len) -> bool {
    sent_chunks.push_back(std::vector<uint8_t>(data, data + len));
    return true;
  });

  // Create a 53-byte payload (e.g. Schedule write packet)
  std::vector<uint8_t> large_packet(53, 0xAA);
  transport.send_command(large_packet);

  // Tick 1: Advance time by 50ms so pacing allows the first chunk, then loop
  mock_millis += 50;
  transport.loop();
  TEST_ASSERT(sent_chunks.size() == 1, "First chunk sent immediately after initial pacing");
  TEST_ASSERT(sent_chunks[0].size() == 20, "First chunk is exactly 20 bytes");

  // Tick 2 (no time passed): Should NOT send second chunk yet due to 50ms pacing
  transport.loop();
  TEST_ASSERT(sent_chunks.size() == 1, "Pacing prevented immediate second chunk");

  // Tick 3 (+51ms): Should send second chunk
  mock_millis += 51;
  transport.loop();
  TEST_ASSERT(sent_chunks.size() == 2, "Second chunk sent after pacing delay");
  // Guard the index: without it the earlier size()==1 assertion leaves cppcheck
  // (correctly) unable to rule out an out-of-bounds read here.
  if (sent_chunks.size() > 1) {
    TEST_ASSERT(sent_chunks[1].size() == 20, "Second chunk is exactly 20 bytes");
  } else {
    TEST_ASSERT(false, "Second chunk is exactly 20 bytes (no second chunk sent)");
  }

  // Tick 4 (+51ms): Should send final 13 bytes
  mock_millis += 51;
  transport.loop();
  TEST_ASSERT(sent_chunks.size() == 3, "Final chunk sent");
  if (sent_chunks.size() > 2) {
    TEST_ASSERT(sent_chunks[2].size() == 13, "Final chunk is exactly 13 bytes");
  } else {
    TEST_ASSERT(false, "Final chunk is exactly 13 bytes (no final chunk sent)");
  }
}


// ---------------------------------------------------------------------------
// Reassembly: a continuation fragment may legitimately begin with 0x24/0x27,
// which are ordinary payload bytes mid-frame. Treating such a fragment as a new
// packet discarded the frame in flight and dispatched the fragment as a runt --
// observed 8 times in the reference captures. These pin the rule and its
// staleness escape hatch.
// ---------------------------------------------------------------------------

// A 27-byte Class 10 frame whose second fragment starts with `lead`.
static std::vector<uint8_t> frame_with_lead_byte(uint8_t lead) {
  std::vector<uint8_t> f{0x24, 0x00, 0x00, 0x07, 0x0A, 0x03,
                         0x00, 0x00, 0xDE, 0x01};
  while (f.size() < 20)
    f.push_back(0x11);          // filler inside the first 20-byte fragment
  f.push_back(lead);            // first byte of the continuation fragment
  while (f.size() < 25)
    f.push_back(0x22);
  f.push_back(0xAA);
  f.push_back(0xBB);            // placeholder, overwritten below
  f[1] = static_cast<uint8_t>(f.size() - 4);
  // A real CRC, stamped after the length byte is final: Transport now drops a
  // bad-CRC frame, and these tests are about reassembly, not corruption. The
  // assertions stay sharp -- if a continuation fragment DID restart
  // reassembly, the resulting runt would fail CRC and be dropped, so the
  // packet count goes to 0 and the test still fails.
  return with_crc(std::move(f));
}

static void test_continuation_fragment_leading_frame_start(uint8_t lead,
                                                           const char *label) {
  esphome::alpha_hwr::core::Transport transport;
  std::vector<std::vector<uint8_t>> packets;
  transport.set_packet_callback([&packets](const uint8_t *d, size_t n) {
    packets.push_back(std::vector<uint8_t>(d, d + n));
  });

  auto frame = frame_with_lead_byte(lead);
  transport.on_notification(frame.data(), 20);
  transport.on_notification(frame.data() + 20, frame.size() - 20);

  TEST_ASSERT(packets.size() == 1, label);
  if (packets.size() == 1) {
    TEST_ASSERT(packets[0].size() == frame.size(),
                "  ...and it is the whole frame, not a runt");
  }
}

static void test_reassembly_continuation_0x24() {
  test_continuation_fragment_leading_frame_start(
      0x24, "Continuation starting 0x24 does not restart reassembly");
}

static void test_reassembly_continuation_0x27() {
  test_continuation_fragment_leading_frame_start(
      0x27, "Continuation starting 0x27 does not restart reassembly");
}

// A frame whose tail never arrives must not swallow the next frame forever.
static void test_reassembly_stale_partial_recovers() {
  esphome::alpha_hwr::core::Transport transport;
  std::vector<std::vector<uint8_t>> packets;
  transport.set_packet_callback([&packets](const uint8_t *d, size_t n) {
    packets.push_back(std::vector<uint8_t>(d, d + n));
  });

  auto truncated = frame_with_lead_byte(0x11);
  transport.on_notification(truncated.data(), 20);   // tail never sent

  mock_millis += 2000;                                // past the staleness bound

  std::vector<uint8_t> whole{0x24, 0x00, 0x00, 0x07, 0x0A, 0x03,
                             0x00, 0x00, 0xDE, 0x01, 0x01, 0xAA, 0xBB};
  whole[1] = static_cast<uint8_t>(whole.size() - 4);
  whole = with_crc(std::move(whole));
  transport.on_notification(whole.data(), whole.size());

  TEST_ASSERT(packets.size() == 1,
              "A stale partial frame is abandoned so the next frame parses");
  if (packets.size() == 1) {
    TEST_ASSERT(packets[0].size() == whole.size(),
                "  ...and the recovered packet is the new frame alone");
  }
}

// ---------------------------------------------------------------------------
// CRC enforcement on the command-response path.
//
// Only telemetry checked the CRC (through parse_frame). The command-response
// path did not, so every control, schedule, single-event, event-log and
// device-info payload -- including the readbacks that decide write verdicts --
// was parsed from bytes nothing had verified.
// ---------------------------------------------------------------------------
static void test_bad_crc_frame_is_dropped() {
  esphome::alpha_hwr::core::Transport transport;
  std::vector<std::vector<uint8_t>> packets;
  transport.set_packet_callback([&packets](const uint8_t *d, size_t n) {
    packets.push_back(std::vector<uint8_t>(d, d + n));
  });

  auto good = frame_with_lead_byte(0x11);
  transport.on_notification(good.data(), good.size());
  TEST_ASSERT(packets.size() == 1, "a frame with a valid CRC is delivered");

  // Same frame, one payload byte flipped, CRC left describing the original.
  auto corrupt = good;
  corrupt[10] ^= 0xFF;
  transport.on_notification(corrupt.data(), corrupt.size());
  TEST_ASSERT(packets.size() == 1, "a frame whose CRC does not match is dropped");

  // And a frame whose CRC bytes themselves are garbage -- the shape every
  // fixture in this suite used to have.
  auto garbage = good;
  garbage[garbage.size() - 2] = 0xAA;
  garbage[garbage.size() - 1] = 0xBB;
  transport.on_notification(garbage.data(), garbage.size());
  TEST_ASSERT(packets.size() == 1, "a frame with a garbage CRC is dropped");
}

// A notification carrying more than one frame's worth of bytes must dispatch
// the first frame at its DECLARED length, not the whole buffer. The completion
// test is `>=`, so the surplus sits in the reassembly buffer; it is outside
// what the CRC covers, so without the trim a perfectly good frame is dropped.
//
// This also fixes a real misdispatch: before the trim, two frames arriving in
// one notification were handed to the callback fused into a single oversized
// packet, with a payload length that described neither.
static void test_trailing_bytes_are_trimmed() {
  esphome::alpha_hwr::core::Transport transport;
  std::vector<std::vector<uint8_t>> packets;
  transport.set_packet_callback([&packets](const uint8_t *d, size_t n) {
    packets.push_back(std::vector<uint8_t>(d, d + n));
  });

  auto first = frame_with_lead_byte(0x11);
  std::vector<uint8_t> two = first;
  two.insert(two.end(), first.begin(), first.end());  // a second whole frame
  transport.on_notification(two.data(), two.size());

  TEST_ASSERT(packets.size() == 1, "a doubled notification yields one packet");
  if (packets.size() == 1) {
    TEST_ASSERT(packets[0].size() == first.size(),
                "  ...trimmed to the declared frame length, not the whole buffer");
  }
}

static void test_bad_crc_cannot_answer_a_command() {
  // The consequence that matters: a corrupt frame must not be taken for the
  // response to a queued command. Asserted in both directions, because a
  // negative-only test here passes for the wrong reason -- a fixture that
  // never matched the queued command in the first place would "prove" the
  // rejection while proving nothing.
  auto run = [](bool corrupt) {
    esphome::alpha_hwr::core::Transport transport;
    transport.set_write_callback([](const uint8_t *, size_t) { return true; });

    int callbacks = 0;
    bool success_reported = false;
    const uint8_t apdu[5] = {0x0A, 0x03, 0x00, 0xDA, 0x01};
    // (expect_type_low_ver, expect_type_high) -- in that order.
    transport.send_apdu_command(apdu, 5, 0xDA01, 0x0000,
      [&](bool success, const uint8_t *, size_t) {
        callbacks++;
        success_reported = success;
      });
    // Chunk pacing: one loop() is not enough to get the command on the wire.
    for (int i = 0; i < 4; i++) { mock_millis += 51; transport.loop(); }


// A Class 10 read response for Sub 0x0000 / Obj 0xDA01 -- the shape the
    // queued command is waiting for.
    std::vector<uint8_t> reply{0x24, 0x00, 0xF8, 0xE7, 0x0A, 0x13,
                               0x00, 0x00, 0xDA, 0x01,
                               0x00, 0x00, 0x0A, 0x01, 0xAA, 0xBB};
    reply[1] = static_cast<uint8_t>(reply.size() - 4);
    reply = with_crc(std::move(reply));
    if (corrupt) reply[reply.size() - 1] ^= 0x01;  // one bit off in the CRC
    transport.on_notification(reply.data(), reply.size());
    return callbacks > 0 && success_reported;
  };

  TEST_ASSERT(run(/*corrupt=*/false),
              "a matching response with a valid CRC completes the command");
  TEST_ASSERT(!run(/*corrupt=*/true),
              "the same response with a bad CRC does not");
}

// The Object 86 Sub 7 mode read -- the most consequential read in the
// component, since it drives control mode, run state, control source and the
// per-mode setpoint cache -- used to pass its two matching arguments in the
// wrong order. The primary comparison therefore never succeeded, and the read
// only ever matched through a "BACKUP MATCH" fallback that existed to absorb
// exactly that mistake. Removing the fallback without fixing the call site
// broke 40 assertions across the write-operation suite.
//
// This pins the ordering directly at the transport, so a future edit that
// swaps them back fails here rather than silently falling into a fallback.
static void test_mode_read_matches_without_fallback() {
  auto run = [](uint16_t low_ver, uint16_t high) {
    esphome::alpha_hwr::core::Transport transport;
    transport.set_write_callback([](const uint8_t *, size_t) { return true; });

    int callbacks = 0;
    bool ok = false;
    const uint8_t apdu[5] = {0x0A, 0x03, 0x56, 0x00, 0x07};
    transport.send_apdu_command(apdu, 5, low_ver, high,
      [&](bool success, const uint8_t *, size_t) { callbacks++; ok = success; });
    for (int i = 0; i < 4; i++) { mock_millis += 51; transport.loop(); }

    // The pump's Sub 7 reply: [00][TypeH=01][TypeL=2F][Ver=01] at bytes 6-9,
    // i.e. Type 303 v1. Transport reads that as 0x0001 / 0x2F01.
    std::vector<uint8_t> reply{0x24, 0x00, 0xF8, 0xE7, 0x0A, 0x0E,
                               0x00, 0x01, 0x2F, 0x01,
                               0x00, 0x00, 0x07, 0x02, 0x00, 0x02,
                               0x00, 0x00, 0x00, 0x00, 0xAA, 0xBB};
    reply[1] = static_cast<uint8_t>(reply.size() - 4);
    reply = with_crc(std::move(reply));
    transport.on_notification(reply.data(), reply.size());
    return callbacks > 0 && ok;
  };

  TEST_ASSERT(run(0x2F01, 0x0001),
              "mode read matches with (type_low_ver, type_high) in that order");
  TEST_ASSERT(!run(0x0001, 0x2F01),
              "  ...and does NOT match when the two are swapped");
}

// The telemetry filter keys off byte 5, which is the APDU body length, not an
// operation code. It therefore drops responses by SIZE. Run ahead of matching
// -- as it used to be -- that discards a reply carrying exactly the type the
// queued command asked for, purely because the body happened to be 20, 43, 45,
// 46, 48 or 9 bytes; the command then times out with its answer in hand. Event
// log entries (20 bytes) hit this for real and were worked around at the call
// site.
//
// The rule these pin: a command that names a type is matched on that type, and
// length gets no vote. Wildcard commands, which have nothing else to go on,
// keep the guard.
static void test_length_collision_does_not_veto_a_type_match() {
  // Deliver a Class 10 reply of the given body length, carrying type 00 00 F4 02
  // (an event-log entry -- the case that actually broke), to a command with the
  // given expectation. Returns true if the command was satisfied.
  auto run = [](uint16_t low_ver, uint16_t high, uint8_t body_len) {
    esphome::alpha_hwr::core::Transport transport;
    transport.set_write_callback([](const uint8_t *, size_t) { return true; });
    int callbacks = 0;
    bool ok = false;
    const uint8_t apdu[5] = {0x0A, 0x03, 0x58, 0x27, 0xD8};
    transport.send_apdu_command(apdu, 5, low_ver, high,
        [&](bool success, const uint8_t *, size_t) { callbacks++; ok = success; });
    for (int i = 0; i < 4; i++) { mock_millis += 51; transport.loop(); }

    std::vector<uint8_t> reply{0x24, 0x00, 0xF8, 0xE7, 0x0A, body_len,
                               0x00, 0x00, 0xF4, 0x02};
    // byte 5 is the body length, so the frame must be body_len + 8 bytes total
    // for the fixture to describe a real pump reply rather than an impossible one.
    while (reply.size() + 2 < static_cast<size_t>(body_len) + 8) reply.push_back(0x00);
    reply.push_back(0xAA);
    reply.push_back(0xBB);
    reply[1] = static_cast<uint8_t>(reply.size() - 4);
    reply = with_crc(std::move(reply));
    TEST_ASSERT(reply.size() == static_cast<size_t>(body_len) + 8,
                "  (fixture is self-consistent: byte5 == total_len - 8)");
    transport.on_notification(reply.data(), reply.size());
    return callbacks > 0 && ok;
  };

  // 0x14 == 20 bytes: on the filter's list, and the real event-log entry size.
  TEST_ASSERT(run(0xF402, 0x0000, 0x14),
              "a 20-byte reply still matches the type the command asked for");
  // 0x2E/0x2D (46/45) bracket the 47-byte cycle-timestamp reply.
  TEST_ASSERT(run(0xF402, 0x0000, 0x2E),
              "  ...and so does a 46-byte one");
  TEST_ASSERT(run(0xF402, 0x0000, 0x30),
              "  ...and a 48-byte one");
  // The guard survives where it is actually needed: a wildcard command has no
  // type to match on, so a telemetry-sized reply must not satisfy it.
  TEST_ASSERT(!run(0x0000, 0x0000, 0x14),
              "a telemetry-sized reply does NOT satisfy a wildcard command");
  TEST_ASSERT(!run(0x0000, 0x0000, 0x30),
              "  ...at either size");
  // ...but a wildcard command still accepts a reply that is not telemetry-sized.
  TEST_ASSERT(run(0x0000, 0x0000, 0x0E),
              "a wildcard command still accepts a non-telemetry-sized reply");
  // And a type mismatch is still a mismatch, at a non-filtered length.
  TEST_ASSERT(!run(0xDE01, 0x0000, 0x0E),
              "a reply of the wrong type still does not match");
}

// ── A caller-supplied timeout is what actually reaches the command ──────────
// Only one production caller passes one (TimeService's 5 s clock read), and it
// is exercised only coarsely. This walked the boundary directly in
// tests/test_auth.cpp until the opening sequence was removed (issue #229), and
// it moved here rather than going with it: the property is the transport's, not
// the caller's, and nothing else in the suite pins it.
void test_a_command_honours_its_own_timeout_not_the_default() {
  std::cout << "\n=== A command honours its own timeout, not the default ===" << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  transport.set_write_callback([](const uint8_t *, size_t) -> bool { return true; });

  const uint32_t started_at = mock_millis;
  int cb_calls = 0;
  bool cb_success = true;
  transport.send_command(std::vector<uint8_t>(10, 0xAA), 0, 0,
                         [&](bool ok, const uint8_t *, size_t) { cb_calls++; cb_success = ok; },
                         /*timeout_ms=*/1000);

  mock_millis += 50;
  transport.loop();   // sends, enters AWAITING_RESPONSE

  mock_millis += 900;
  transport.loop();
  TEST_ASSERT(cb_calls == 0, "900 ms in, the 1000 ms window has not expired");

  mock_millis += 200;
  transport.loop();
  TEST_ASSERT(cb_calls == 1, "past 1000 ms the command gives up exactly once");
  TEST_ASSERT(!cb_success, "...and reports failure");

  // The point of the case: 1100 ms is well short of the 3000 ms default, so a
  // build that ignored the argument would still be waiting here.
  TEST_ASSERT(mock_millis - started_at < 3000,
              "and it did so well short of the 3000 ms default, which is what "
              "shows the caller's value was the one in force");
}

// ── What the receiver will accept as a frame at all (issue #278) ────────────

// 0x27 begins every frame we SEND and no frame the pump sends. Across the 44,200
// CRC-valid frames of the capture corpus, all 22,138 phone->pump frames begin
// 0x27 and all 22,062 pump->phone frames begin 0x24; not one inbound frame
// begins 0x27. on_notification() is fed GATT notifications only, so it never
// sees our own writes.
//
// It used to accept it, on the strength of a comment calling 0x27 "also echoed
// back". That is the byte issue #259's corrupt fragment begins with.
void test_an_inbound_frame_never_starts_with_the_request_delimiter() {
  std::cout << "\n=== 0x27 does not begin an inbound frame ===" << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  std::vector<std::vector<uint8_t>> packets;
  transport.set_packet_callback([&packets](const uint8_t *d, size_t n) {
    packets.push_back(std::vector<uint8_t>(d, d + n));
  });

  // A frame that is valid in every other respect -- real length, real CRC --
  // and differs from an acceptable one only in its delimiter.
  std::vector<uint8_t> f{0x27, 0x00, 0xF8, 0xE7, 0x0A, 0x03, 0x00, 0x00, 0xDE, 0x01};
  while (f.size() < 22) f.push_back(0x11);
  f.push_back(0x00);
  f.push_back(0x00);
  f[1] = static_cast<uint8_t>(f.size() - 4);
  f = with_crc(std::move(f));

  transport.on_notification(f.data(), f.size());

  TEST_ASSERT(packets.empty(),
              "a CRC-valid frame arriving with the request delimiter is not "
              "reassembled -- the pump does not send that byte first");
  TEST_ASSERT(!transport.is_reassembling(),
              "  ...and it does not leave reassembly armed for whatever "
              "arrives next");
}

// The length byte bounds the same field from below. The floor is 4, not 5:
// 5 is the corpus minimum, but the corpus is the phone app's traffic and the app
// is never refused, so its minimum is a minimum over non-refusal frames only.
// A zero-payload Unknown Class refusal declares 4.
void test_a_frame_start_declaring_less_than_a_telegram_is_refused() {
  std::cout << "\n=== a length byte below the floor does not start a frame ==="
            << std::endl;

  // The exact first fragment from issue #259's report. Its length byte is 0, so
  // the expected length came out as 4, the 20-byte notification satisfied the
  // completion test immediately, the frame was trimmed to four bytes and failed
  // CRC -- having consumed the frame-start slot, so the two real continuations
  // behind it had nowhere to go.
  const std::vector<uint8_t> reported_fragment = {
      0x27, 0x00, 0x11, 0xFF, 0xFF, 0x00, 0x07, 0x00, 0x83, 0xFF,
      0xFF, 0x00, 0x0D, 0x00, 0xB3, 0x00, 0xB3, 0x11, 0x43, 0x34};

  for (uint8_t lead : {(uint8_t) 0x24, (uint8_t) 0x27}) {
    esphome::alpha_hwr::core::Transport transport;
    std::vector<std::vector<uint8_t>> packets;
    transport.set_packet_callback([&packets](const uint8_t *d, size_t n) {
      packets.push_back(std::vector<uint8_t>(d, d + n));
    });

    std::vector<uint8_t> frag = reported_fragment;
    frag[0] = lead;   // 0x24 too, so this is about the LENGTH and not the byte
    transport.on_notification(frag.data(), frag.size());

    // Hoisted rather than written as a ternary inside the macro: TEST_ASSERT
    // does not parenthesise its argument, so `<< a == b ? x : y` binds as
    // `(cout << a) == b`.
    const char *what = (lead == 0x24)
                           ? "a declared length of 0 does not start a frame (0x24)"
                           : "a declared length of 0 does not start a frame (0x27)";
    TEST_ASSERT(packets.empty() && !transport.is_reassembling(), what);
  }

  // ...and the floor stops exactly where the protocol does. An 8-byte Unknown
  // Class refusal declares 4 and must still be received: it is the shape that
  // only ever appears in a refusal, which is why the corpus does not contain it.
  {
    esphome::alpha_hwr::core::Transport transport;
    std::vector<std::vector<uint8_t>> packets;
    transport.set_packet_callback([&packets](const uint8_t *d, size_t n) {
      packets.push_back(std::vector<uint8_t>(d, d + n));
    });
    auto refusal = with_crc({0x24, 0x04, 0xF8, 0xE7, 0x0A, 0x40, 0x00, 0x00});
    transport.on_notification(refusal.data(), refusal.size());
    TEST_ASSERT(packets.size() == 1,
                "  ...and a length of 4 -- an 8-byte Unknown Class refusal -- is "
                "still a frame, which is where a floor of 5 would have broken it");
  }

  // The cost of having no floor, which is NOT visible on the fragment itself.
  //
  // A sub-minimum declaration that arrives complete is caught by the CRC a
  // moment later either way, so the two behaviours are indistinguishable there
  // -- which is why the assertions above pass with the floor deleted, and why
  // this case has to exist. The damage is to the frame BEHIND it: a short
  // notification declaring a length it has not reached leaves reassembly armed,
  // and the next real frame is appended to it as a continuation. Two frames are
  // then lost rather than one, and the second was perfectly good.
  {
    esphome::alpha_hwr::core::Transport transport;
    std::vector<std::vector<uint8_t>> packets;
    transport.set_packet_callback([&packets](const uint8_t *d, size_t n) {
      packets.push_back(std::vector<uint8_t>(d, d + n));
    });

    // Declares 3, one below the floor, and stops after three bytes so it does
    // not satisfy its own expected length of 7.
    //
    // 3 rather than 1, and that one byte is the whole assertion. A fixture
    // declaring 1 clears any floor of 2 or more, so it says nothing about where
    // the floor SITS -- the suite stayed green with MIN_LENGTH_FIELD set to 3
    // and to 2, while the change's own prose argued at length about which value
    // was right. At 3 the fixture is admitted by exactly the floors that are too
    // low and refused by the correct one.
    const std::vector<uint8_t> runt = {0x24, 0x03, 0xAA};
    transport.on_notification(runt.data(), runt.size());

    auto good = with_crc({0x24, 0x05, 0xF8, 0xE7, 0x0A, 0x01, 0x00, 0x00, 0x00});
    transport.on_notification(good.data(), good.size());

    TEST_ASSERT(packets.size() == 1,
                "the frame arriving behind a sub-minimum declaration is "
                "received -- without the floor it is swallowed as that "
                "declaration's continuation");
    if (packets.size() == 1) {
      TEST_ASSERT(packets[0].size() == good.size(),
                  "  ...whole, rather than as the tail of something else");
    }
  }
}



// A frame start delivered ALONE, before its length byte exists.
//
// The floor cannot judge this one -- it tests data[1], and there is no data[1]
// yet -- so reassembly is armed with an expected length of 0. The completion
// test requires a non-zero expected length, so unless a later fragment supplies
// it, nothing can ever complete and every notification that follows is swallowed
// as a continuation until the staleness guard expires a second later.
void test_a_lone_frame_start_byte_does_not_swallow_what_follows() {
  std::cout << "\n=== a one-byte frame start still learns its length ==="
            << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  std::vector<std::vector<uint8_t>> packets;
  transport.set_packet_callback([&packets](const uint8_t *d, size_t n) {
    packets.push_back(std::vector<uint8_t>(d, d + n));
  });

  const uint8_t lone = 0x24;
  transport.on_notification(&lone, 1);

  // The rest of that same frame, arriving as the next notification.
  auto whole = with_crc({0x24, 0x05, 0xF8, 0xE7, 0x0A, 0x01, 0x00, 0x00, 0x00});
  transport.on_notification(whole.data() + 1, whole.size() - 1);

  TEST_ASSERT(packets.size() == 1,
              "the frame completes once the length byte arrives, rather than "
              "waiting on an expected length of zero that can never be met");
  if (packets.size() == 1) {
    TEST_ASSERT(packets[0].size() == whole.size(),
                "  ...and is the whole frame");
  }
}

// Two frames sharing one notification, where the first is near the size limit.
//
// The completion test is `>=` so that trailing bytes are trimmed rather than
// lost. The overflow guard runs BEFORE it, so without a "still incomplete" term
// the guard throws away a complete, CRC-valid frame for the sake of bytes that
// were never part of it.
void test_trailing_bytes_do_not_overflow_a_frame_that_is_already_complete() {
  std::cout << "\n=== a complete frame is trimmed, not overflowed ==="
            << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  std::vector<std::vector<uint8_t>> packets;
  transport.set_packet_callback([&packets](const uint8_t *d, size_t n) {
    packets.push_back(std::vector<uint8_t>(d, d + n));
  });

  // A 259-byte frame -- the largest legal one, so the cap is exactly its size --
  // delivered with one extra byte riding along at the end.
  std::vector<uint8_t> f{0x24, 0x00, 0xF8, 0xE7, 0x0A, 0x03, 0x00, 0x00, 0xDE, 0x01};
  while (f.size() < 257) f.push_back(0x11);
  f.push_back(0x00);
  f.push_back(0x00);
  f[1] = static_cast<uint8_t>(f.size() - 4);
  f = with_crc(std::move(f));
  f.push_back(0x99);   // the head of whatever came next

  size_t off = 0;
  while (off < f.size()) {
    const size_t n = std::min<size_t>(20, f.size() - off);
    transport.on_notification(f.data() + off, n);
    off += n;
  }

  TEST_ASSERT(packets.size() == 1,
              "the completed frame is delivered rather than discarded as an "
              "overflow caused by a byte belonging to the frame behind it");
  if (packets.size() == 1) {
    TEST_ASSERT(packets[0].size() == 259,
                "  ...trimmed to its declared length, which is what the "
                "completion test's `>=` exists for");
  }
}

// ── An inbound overflow is a loss of frame sync, not a disconnect ───────────
// on_notification() gives up on a partial frame once it passes MAX_PACKET_SIZE.
// That call used to be reset(), which cancels the command queue -- so a single
// corrupt fragment declaring a long frame could strand every read in flight on
// a LIVE link, with nothing telling any caller. That is the reachability issue
// #259 named, and these pin the two paths apart: the commands are still
// outstanding, the pump may still answer them, and if it does not, each one's
// own timeout says so.

/// Feed a partial frame that declares more bytes than the buffer will hold, and
/// stop in the one place where the overflow guard is the only thing that can end
/// it. No time passes, so the staleness guard is not in play either.
///
/// The sizes are exact and they have to be. The buffer's cap is
/// MAX_TELEGRAM_LEN, 259, so a partial frame only overflows once it passes that
/// -- and it must not have completed first, or the completion test fires
/// instead, the frame is dispatched, CRC-rejected and cleared, and every
/// assertion below passes with the overflow guard disabled. A first draft did
/// exactly that and the mutation survived.
///
/// So the declared length has to be one the buffer can EXCEED without reaching:
/// 255 (a 259-byte frame) cannot be, since 259 is also the cap. Declare a frame
/// the fragments overshoot instead -- 251 bytes -- and stop at 260: past the cap,
/// past the declared length, and never completing because the trim-and-dispatch
/// only runs for a frame that has not already been thrown away.
///
/// This helper has now been wrong twice, in both directions, and both times the
/// mutation check is what said so.
static void overflow_the_reassembly_buffer(
    esphome::alpha_hwr::core::Transport &transport) {
  std::vector<uint8_t> head(20, 0x11);
  head[0] = 0x24;
  head[1] = 0xF7;   // 247 + 4 = a 251-byte frame
  transport.on_notification(head.data(), head.size());
  const std::vector<uint8_t> more(20, 0x22);
  for (int i = 0; i < 12; i++) {          // 20 + 12*20 = 260, past the 259 cap
    transport.on_notification(more.data(), more.size());
  }
}

// The ceiling, from the side that matters: a maximum-length LEGAL frame has to
// survive reassembly. The overflow guard enforces the ceiling, and until this
// nothing asserted where it SITS -- the overflow probe below is over the cap at
// any of the candidate values, so moving the constant changed nothing any test
// could see.
//
// 259 = LENGTH 255 + 4, and LENGTH counts DA + SA + PDU. That is the largest
// telegram the specification permits and the same bound the vendor's own builder
// enforces (GeniBuilder rejects a length field above 255). A brief detour
// through 257 in this change came from reading the length field as bounded by
// MAX_PDU_LEN alone; see frame_builder.h.
void test_a_maximum_length_legal_frame_is_not_read_as_an_overflow() {
  std::cout << "\n=== a 259-byte frame is legal and must survive ===" << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  std::vector<std::vector<uint8_t>> packets;
  transport.set_packet_callback([&packets](const uint8_t *d, size_t n) {
    packets.push_back(std::vector<uint8_t>(d, d + n));
  });

  std::vector<uint8_t> f{0x24, 0x00, 0xF8, 0xE7, 0x0A, 0x03, 0x00, 0x00, 0xDE, 0x01};
  while (f.size() < 257) f.push_back(0x11);
  f.push_back(0x00);
  f.push_back(0x00);                                   // CRC placeholders
  f[1] = static_cast<uint8_t>(f.size() - 4);           // 255, the field's maximum
  f = with_crc(std::move(f));

  // Delivered the way the pump delivers, 20 bytes of ATT payload at a time.
  size_t off = 0;
  while (off < f.size()) {
    const size_t n = std::min<size_t>(20, f.size() - off);
    transport.on_notification(f.data() + off, n);
    off += n;
  }

  TEST_ASSERT(packets.size() == 1,
              "it reassembles and is dispatched, rather than being discarded as "
              "an overflow short of the protocol's own limit");
  if (packets.size() == 1) {
    TEST_ASSERT(packets[0].size() == 259, "  ...whole, all 259 bytes of it");
  }
}

void test_an_inbound_overflow_does_not_cancel_a_command_in_flight() {
  std::cout << "\n=== an inbound overflow leaves the queue alone ===" << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  transport.set_write_callback([](const uint8_t *, size_t) -> bool { return true; });

  int cb_calls = 0;
  bool ok_reported = true;
  transport.send_command(std::vector<uint8_t>(10, 0xAA), 0, 0,
                         [&](bool ok, const uint8_t *, size_t) {
                           cb_calls++;
                           ok_reported = ok;
                         },
                         /*timeout_ms=*/1000);
  mock_millis += 50;
  transport.loop();   // on the wire, awaiting a response

  overflow_the_reassembly_buffer(transport);
  transport.loop();

  TEST_ASSERT(cb_calls == 0,
              "the command is still outstanding -- losing inbound frame sync "
              "says nothing about whether the pump will answer it");
  TEST_ASSERT(!transport.is_reassembling() && transport.get_buffer_size() == 0,
              "  ...and the partial frame itself is gone, which is the part "
              "that had to happen");

  // And it still has the timeout it always had, which is how its caller finds
  // out if the frame that overflowed WAS the reply it was waiting for.
  mock_millis += 1100;
  transport.loop();
  TEST_ASSERT(cb_calls == 1 && !ok_reported,
              "  ...and it reports failure on its own timeout, through the path "
              "every caller already handles");
}

// The reply debt survives an overflow too, and unlike the resync hold this one
// is a genuine REVERSAL rather than a workaround being removed. The old code
// reached this path through reset(), which cleared the debt deliberately -- its
// comment argued that clearing was "the safer of the two errors".
//
// It is not. Clearing the debt lets a reply owed by an abandoned command be
// taken for the acknowledgement of the NEXT write, which is the misattribution
// issue #248 exists to prevent; keeping it costs at most one suppressed
// acknowledgement, and every awaited Class 10 SET confirms by reading the value
// back rather than trusting that acknowledgement. Losing inbound frame sync is
// not evidence that the pump has stopped owing us an answer.
//
// Without this the mutation is invisible: a skeptic put the old clear back into
// the overflow branch and all 31 test binaries passed.
void test_an_inbound_overflow_keeps_the_reply_debt() {
  std::cout << "\n=== an inbound overflow does not forgive the reply debt ==="
            << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  transport.set_write_callback([](const uint8_t *, size_t) -> bool { return true; });

  // A temperature-range write that goes unanswered: on timeout the pump still
  // owes it a reply, and that debt is what the next acknowledgement pays off.
  const std::vector<uint8_t> req = {0x27, 0x0B, 0xE7, 0xF8, 0x0A, 0x97,
                                    0x5B, 0x01, 0xAE, 0x03, 0x00, 0x00};
  int first = 0;
  transport.send_command(req, 0, 0,
                         [&](bool, const uint8_t *, size_t) { first++; },
                         esphome::alpha_hwr::core::Transport::SET_ACK_TIMEOUT_MS,
                         false, /*expect_short_ack=*/true, /*quiet_timeout=*/true);
  mock_millis += 50;
  transport.loop();
  mock_millis += esphome::alpha_hwr::core::Transport::SET_ACK_TIMEOUT_MS + 10;
  transport.loop();
  TEST_ASSERT(first == 1, "the first write gave up, so a reply is owed");

  overflow_the_reassembly_buffer(transport);

  // The next write, and an acknowledgement arriving well inside the window.
  int second = 0;
  bool second_ok = false;
  transport.send_command(req, 0, 0,
                         [&](bool ok, const uint8_t *, size_t) {
                           second++;
                           second_ok = ok;
                         },
                         esphome::alpha_hwr::core::Transport::SET_ACK_TIMEOUT_MS,
                         false, /*expect_short_ack=*/true, /*quiet_timeout=*/true);
  mock_millis += 60;
  transport.loop();
  const std::vector<uint8_t> ack = {0x24, 0x05, 0xF8, 0xE7, 0x0A, 0x01, 0x00, 0xAE, 0xA2};
  transport.on_notification(ack.data(), ack.size());

  TEST_ASSERT(second == 0,
              "the ambiguous acknowledgement is withheld -- the overflow did "
              "not wipe the debt that says it might belong to the write before");

  // And the second write still ends, on its own timeout, reporting failure.
  mock_millis += esphome::alpha_hwr::core::Transport::SET_ACK_TIMEOUT_MS + 10;
  transport.loop();
  TEST_ASSERT(second == 1 && !second_ok,
              "  ...and the write it was withheld from reports failure rather "
              "than hanging");
}

void test_an_inbound_overflow_keeps_the_peer_resync_hold() {
  std::cout << "\n=== an inbound overflow does not release the resync hold ==="
            << std::endl;
  esphome::alpha_hwr::core::Transport transport;

  int chunks = 0;
  std::vector<uint8_t> first_bytes;
  transport.set_write_callback([&](const uint8_t *data, size_t len) -> bool {
    chunks++;
    if (len > 0) first_bytes.push_back(data[0]);
    return chunks != 2;  // chunk 1 lands, chunk 2 fails: a partial at the peer
  });

  transport.send_command(std::vector<uint8_t>(53, 0xAA), 0, 0, nullptr);
  transport.send_command(std::vector<uint8_t>(10, 0xBB), 0, 0, nullptr);
  for (int i = 0; i < 2; i++) { mock_millis += 51; transport.loop(); }
  TEST_ASSERT(chunks == 2, "the second chunk failed, so the hold is armed");

  overflow_the_reassembly_buffer(transport);

  // Same boundary as test_partial_write_holds_off_so_the_peer_can_resync(), for
  // the same reason. The old code reached this through reset(), which clears the
  // hold, and had to save and restore it by hand around the call.
  for (uint32_t t = 0; t < 950; t++) { mock_millis += 1; transport.loop(); }
  bool sent_during_hold = false;
  for (uint8_t b : first_bytes) if (b == 0xBB) sent_during_hold = true;
  TEST_ASSERT(!sent_during_hold,
              "the next command is still withheld -- an inbound overflow does "
              "not tell us the peer stopped holding our partial frame");

  for (uint32_t t = 0; t < 400; t++) { mock_millis += 1; transport.loop(); }
  for (uint8_t b : first_bytes) if (b == 0xBB) sent_during_hold = true;
  TEST_ASSERT(sent_during_hold, "  ...and it goes out once the hold elapses");
}

// ── reset() fails what it abandons, rather than dropping it ─────────────────
// This block used to pin the opposite -- a hazard, asserted so it stayed
// visible: reset() cleared the queue silently, so a service with a read in
// flight heard nothing ever again, no success, no failure, and no timeout
// because the timeout went with the queue entry. The opening sequence carried a
// whole-sequence backstop for exactly that, and it went away with the sequence
// (issue #229 -- not #174, which is the issue that put the sequence on a
// reply-driven footing in the first place, and whose fix CREATED the backstop),
// leaving nothing (issue #259).
void test_reset_fails_a_pending_command_instead_of_dropping_it() {
  std::cout << "\n=== reset() fails a pending command ===" << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  transport.set_write_callback([](const uint8_t *, size_t) -> bool { return true; });

  int cb_calls = 0;
  bool reported_success = true;
  transport.send_command(std::vector<uint8_t>(10, 0xAA), 0, 0,
                         [&](bool ok, const uint8_t *, size_t) {
                           cb_calls++;
                           reported_success = ok;
                         },
                         /*timeout_ms=*/1000);

  mock_millis += 50;
  transport.loop();   // in flight, awaiting a response

  transport.reset();

  TEST_ASSERT(cb_calls == 1,
              "the abandoned command's callback fires, so the caller learns the "
              "read is over instead of waiting for a reply the link can no "
              "longer carry");
  TEST_ASSERT(!reported_success,
              "  ...and it fires with failure, the same verdict a timeout gives");

  mock_millis += 10000;   // ten times the command's own window
  transport.loop();
  TEST_ASSERT(cb_calls == 1, "  ...exactly once; nothing fires it again later");
}

// A command that has not been sent yet is owed the same answer as one in
// flight. It never reached AWAITING_RESPONSE, so it never had a timeout of its
// own to fall back on -- it is the case with no other way out at all.
void test_reset_fails_a_command_that_never_went_out() {
  std::cout << "\n=== reset() fails an unsent command too ===" << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  transport.set_write_callback([](const uint8_t *, size_t) -> bool { return true; });

  int first = 0, second = 0;
  transport.send_command(std::vector<uint8_t>(10, 0xAA), 0, 0,
                         [&](bool, const uint8_t *, size_t) { first++; }, 1000);
  transport.send_command(std::vector<uint8_t>(10, 0xBB), 0, 0,
                         [&](bool, const uint8_t *, size_t) { second++; }, 1000);

  mock_millis += 50;
  transport.loop();   // only the first one is on the wire

  transport.reset();

  TEST_ASSERT(first == 1 && second == 1,
              "both the in-flight command and the one still queued behind it "
              "report failure");
}

// The reason the drain exists rather than a plain loop over the queue: a read
// chain continues past a failed step by sending the next read from inside the
// callback. Those land in the queue reset() has just emptied, and if they are
// left there they are precisely the thing clearing the queue was meant to
// prevent -- a write from the dead connection running on the next one.
void test_reset_takes_the_commands_its_own_callbacks_queue() {
  std::cout << "\n=== reset() unwinds a chain that re-sends on failure ==="
            << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  std::vector<std::vector<uint8_t>> writes;
  transport.set_write_callback([&writes](const uint8_t *d, size_t n) -> bool {
    writes.push_back(std::vector<uint8_t>(d, d + n));
    return true;
  });

  // A four-step chain, shaped like HistoryService's: each failure sends the
  // next read, and the last step reports the whole read complete.
  int steps = 0;
  bool chain_finished = false;
  std::function<void(int)> read_next = [&](int idx) {
    if (idx >= 4) {
      chain_finished = true;
      // The terminal branch queues one more command, because real ones do:
      // ControlService::sync_cache_async() calls read_setpoint_ranges() after
      // it has already reported the sync complete. Without it nothing was ever
      // going to be written after the drain, and the "nothing was sent
      // afterwards" assertion below could not fail for any reason at all.
      transport.send_command(std::vector<uint8_t>(10, 0xEE), 0, 0,
                             [&](bool, const uint8_t *, size_t) {}, 1000);
      return;
    }
    steps++;
    transport.send_command(std::vector<uint8_t>(10, (uint8_t) idx), 0, 0,
                           [&, idx](bool, const uint8_t *, size_t) {
                             read_next(idx + 1);
                           },
                           1000);
  };
  read_next(0);

  mock_millis += 50;
  transport.loop();
  const size_t writes_before = writes.size();

  transport.reset();

  TEST_ASSERT(steps == 4,
              "the chain walked all four steps rather than stalling on the one "
              "that was in flight");
  TEST_ASSERT(chain_finished,
              "  ...and reached its terminal branch, which is where a caller's "
              "on_complete lives");

  // Nothing the unwind queued may still be sitting there waiting for a link.
  mock_millis += 5000;
  for (int i = 0; i < 20; i++) {
    mock_millis += 51;
    transport.loop();
  }
  TEST_ASSERT(writes.size() == writes_before,
              "  ...and not one of the commands it queued while unwinding was "
              "sent afterwards");
}

// A callback that resets the transport it is being reset by.
void test_reset_from_inside_an_abandoned_callback_does_not_double_fire() {
  std::cout << "\n=== reset() re-entered from a callback ===" << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  transport.set_write_callback([](const uint8_t *, size_t) -> bool { return true; });

  int a = 0, b = 0;
  transport.send_command(std::vector<uint8_t>(10, 0xAA), 0, 0,
                         [&](bool, const uint8_t *, size_t) {
                           a++;
                           transport.reset();   // re-entrant
                         },
                         1000);
  transport.send_command(std::vector<uint8_t>(10, 0xBB), 0, 0,
                         [&](bool, const uint8_t *, size_t) { b++; }, 1000);

  mock_millis += 50;
  transport.loop();

  transport.reset();

  TEST_ASSERT(a == 1 && b == 1,
              "each abandoned callback fires exactly once even though one of "
              "them called reset() again");
}

// ...and the case the one above does NOT reach, which is the case that matters.
//
// The test above resets from a callback that queued nothing, so the nested call
// returns at abandon_queue_()'s emptiness check and the `abandoning_` guard is
// never consulted. That made it a passing test of a line it did not touch: with
// the guard deleted, it still passes.
//
// A read chain does the opposite. It sends the next command FIRST and could
// reach reset() after -- and then the queue is not empty, the emptiness check
// does not fire, and the guard is the only thing between this and one recursive
// drain per step. Nothing else bounds it: MAX_ABANDON_STEPS is counted per
// call, so each nested drain starts its own count from zero. Without the guard
// this same shape dies of stack exhaustion; a skeptic reproduced the SIGSEGV.
//
// The assertion also pins the cap's VALUE. `steps <= 600` would pass for a cap
// of 8, and a cap of 8 would strand every read chain longer than eight commands
// -- the exact failure this whole change exists to fix.
void test_a_callback_that_queues_then_resets_does_not_recurse_per_step() {
  std::cout << "\n=== a callback that queues, then resets ===" << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  transport.set_write_callback([](const uint8_t *, size_t) -> bool { return true; });

  int fired = 0;
  std::function<void()> queue_step = [&]() {
    transport.send_command(std::vector<uint8_t>(10, 0xAA), 0, 0,
                           [&](bool, const uint8_t *, size_t) {
                             fired++;
                             queue_step();       // the next read of the chain...
                             transport.reset();  // ...and only then the reset
                           },
                           1000);
  };
  queue_step();

  mock_millis += 50;
  transport.loop();

  transport.reset();

  TEST_ASSERT(fired == 512,
              "the drain unwound iteratively to exactly MAX_ABANDON_STEPS, "
              "rather than recursing one frame per step until the stack ran out");
}

// A callback that resets on the SUCCESS path. Nothing about having been answered
// makes a callback safe to run while its own queue entry is still reachable, and
// the first cut of this change converted only the failure paths -- leaving the
// three completions in try_dispatch_response() invoking a callback that reset()
// could then find at the head and invoke a second time.
void test_a_success_callback_that_resets_does_not_re_enter_itself() {
  std::cout << "\n=== a success callback calls reset() ===" << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  transport.set_write_callback([](const uint8_t *, size_t) -> bool { return true; });

  // A temperature-range write, one of the sends that awaits a short ACK.
  std::vector<uint8_t> req = {0x27, 0x0B, 0xE7, 0xF8, 0x0A, 0x97,
                              0x5B, 0x01, 0xAE, 0x03, 0x00, 0x00};

  int a = 0, b = 0;
  bool every_verdict_was_the_real_one = true;
  transport.send_command(req, 0, 0,
                         [&](bool ok, const uint8_t *, size_t) {
                           a++;
                           if (!ok) every_verdict_was_the_real_one = false;
                           transport.reset();
                         },
                         esphome::alpha_hwr::core::Transport::SET_ACK_TIMEOUT_MS,
                         /*allow_register_read=*/false, /*expect_short_ack=*/true,
                         /*quiet_timeout=*/true);
  transport.send_command(std::vector<uint8_t>(10, 0xBB), 0, 0,
                         [&](bool, const uint8_t *, size_t) { b++; }, 1000);

  mock_millis += 50;
  transport.loop();

  const std::vector<uint8_t> ack = {0x24, 0x05, 0xF8, 0xE7, 0x0A, 0x01, 0x00, 0xAE, 0xA2};
  transport.on_notification(ack.data(), ack.size());

  TEST_ASSERT(a == 1, "the answered command's callback runs once, not once for "
                      "the acknowledgement and again for the reset it called");
  TEST_ASSERT(every_verdict_was_the_real_one,
              "  ...and keeps the verdict it earned, rather than being told "
              "afterwards that the write it just saw acknowledged had failed");
  TEST_ASSERT(b == 1,
              "  ...and the command queued behind it is failed by that reset");
}

// The same re-entrancy from the other direction, and the one that actually
// changed with issue #259. A callback invoked by loop() -- on a timeout, or on a
// write failure -- may reach reset(). Before, that just cleared the queue out
// from under loop(), which then ran pop_front() on it. Now reset() FAILS the
// queue, so if loop() still had this command at the front, reset() would invoke
// the very callback it was called from, a second time and re-entrantly.
void test_a_timeout_callback_that_resets_does_not_re_enter_itself() {
  std::cout << "\n=== a timeout callback calls reset() ===" << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  transport.set_write_callback([](const uint8_t *, size_t) -> bool { return true; });

  int a = 0, b = 0;
  transport.send_command(std::vector<uint8_t>(10, 0xAA), 0, 0,
                         [&](bool, const uint8_t *, size_t) {
                           a++;
                           transport.reset();
                         },
                         /*timeout_ms=*/500);
  transport.send_command(std::vector<uint8_t>(10, 0xBB), 0, 0,
                         [&](bool, const uint8_t *, size_t) { b++; }, 500);

  mock_millis += 50;
  transport.loop();     // the first is on the wire
  mock_millis += 600;
  transport.loop();     // ...and times out, and its callback resets

  TEST_ASSERT(a == 1,
              "the timing-out command's callback runs once, not once from the "
              "timeout and again from the reset it called");
  TEST_ASSERT(b == 1,
              "  ...and the command queued behind it is failed by that reset, "
              "which is the whole point of it");
}

// The drain is bounded. A chain that answers every failure with another send
// would otherwise spin here until the task watchdog fires -- a panic in place of
// a stranded read, which is the worse of the two.
void test_a_chain_that_never_stops_re_sending_hits_the_cap() {
  std::cout << "\n=== an endless re-send chain is capped, not spun ==="
            << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  transport.set_write_callback([](const uint8_t *, size_t) -> bool { return true; });

  int steps = 0;
  std::function<void()> again = [&]() {
    steps++;
    transport.send_command(std::vector<uint8_t>(10, 0xAA), 0, 0,
                           [&](bool, const uint8_t *, size_t) { again(); }, 1000);
  };
  again();

  mock_millis += 50;
  transport.loop();

  transport.reset();   // must return, and must not recurse into the stack

  // Exactly, not a range. `steps <= 600` passes for a cap of 8 and passes at
  // steps == 2, which is what the drain does with its re-queue loop deleted --
  // so a loose bound here made this test blind to two separate defects.
  // 513 = the initial send plus MAX_ABANDON_STEPS unwound.
  TEST_ASSERT(steps == 513,
              "the drain stopped at exactly its cap instead of running forever");
}

// ── A refused Class 10 write reports failure, not success or silence ────────
// The short-ACK branch in try_dispatch_response() exists because some Class 10
// SET commands are answered with a 9-byte frame carrying no Obj/Sub fields. It
// used to key on the whole head byte against {0x01, 0x81} and derive success
// from the byte AFTER it, read as an error code. Both halves were wrong
// (issue #208):
//
//   - 0x81 is acknowledge 10, Unknown Data Item. The byte after it is the ID of
//     the item the pump did not recognise, so "err_code == 0x00" meant "the
//     unknown item's ID was zero" -- and the frame captured on hardware is
//     exactly `... 0A 81 00 ...`, so that refusal was reported as accepted.
//   - A 0xC1 (Illegal Operation) or 0x41 (Unknown Class) refusal matched
//     nothing at all, fell past this branch and past the len >= 11 floor, and
//     failed by 3 s timeout -- logging "no response" about a pump that had
//     answered in milliseconds.
//
// Driven through the real Transport rather than the pure predicate, because
// what regressed was the dispatch decision, not the arithmetic.
static void queue_a_class10_temperature_write(esphome::alpha_hwr::core::Transport &t,
                                              int *cb_calls, bool *cb_success) {
  // OpSpec 0x97 with the Obj 91 / Sub 430 address -- the temperature-range
  // write. `expect_short_ack` is what admits it to the short-ACK branch: since
  // issue #253 that is the caller's declaration rather than a list of addresses
  // the transport recognises.
  std::vector<uint8_t> req = {0x27, 0x0B, 0xE7, 0xF8, 0x0A, 0x97, 91, 0x01, 0xAE, 0x00, 0x00, 0x00};
  t.send_command(req, 0, 0,
                 [cb_calls, cb_success](bool ok, const uint8_t *, size_t) {
                   (*cb_calls)++;
                   *cb_success = ok;
                 },
                 /*timeout_ms=*/3000, /*allow_register_read=*/false,
                 /*expect_short_ack=*/true);
  mock_millis += 50;
  t.loop();
}

// ── One late reply costs one match, and the guard converges ────────────────
// Issue #248. Nothing cancels a request in GENIbus, so a command that timed out
// is still owed an answer -- and that answer is byte-identical to the
// acknowledgement the NEXT Class 10 write is waiting for. Transport records the
// debt and lets the next ambiguous frame settle it.
//
// The trap this pins is what the FIRST design did instead. It suppressed on a
// time window and let the frame fall through, so the debt was never paid: the
// suppressed command timed out, its timeout re-armed the window, and the next
// acknowledgement landed inside the new one. With a pump answering in ~50 ms and
// pacing at 50, that closes on itself -- four consecutive writes failed against
// a perfectly healthy pump in the harness that found it. A count that is paid
// down, plus `suppressed_a_frame` so the paid command does not re-open the
// account, is what makes the sequence terminate.
static void pump(esphome::alpha_hwr::core::Transport &t, int iterations) {
  for (int i = 0; i < iterations; i++) {
    mock_millis += 20;
    t.loop();
  }
}

void test_a_late_reply_costs_one_match_and_no_more() {
  std::cout << "\n=== One stale reply costs one match, then matching resumes ===" << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  transport.set_write_callback([](const uint8_t *, size_t) -> bool { return true; });
  const std::vector<uint8_t> ack = with_crc({0x24, 0x05, 0xF8, 0xE7, 0x0A, 0x01, 0x00, 0x00, 0x00});

  // One command expects an answer and never gets one, so the pump owes a reply.
  int lost = 0;
  transport.send_command(std::vector<uint8_t>(10, 0xAA), 91, 430,
                         [&](bool, const uint8_t *, size_t) { lost++; },
                         /*timeout_ms=*/500);
  pump(transport, 3);
  mock_millis += 600;
  transport.loop();
  TEST_ASSERT(lost == 1, "the first command gave up, and is still owed a reply");

  // Four writes follow, each promptly acknowledged by a healthy pump. The first
  // pays off the debt; every one after it must be matched normally.
  int settled = 0, succeeded = 0;
  for (int i = 0; i < 4; i++) {
    int calls = 0;
    bool ok = false;
    queue_a_class10_temperature_write(transport, &calls, &ok);
    pump(transport, 3);
    transport.on_notification(ack.data(), ack.size());
    transport.loop();
    if (calls == 0) {           // not matched -- run out its own timeout
      mock_millis += 3100;
      transport.loop();
    }
    settled++;
    if (ok) succeeded++;
  }
  TEST_ASSERT(settled == 4, "all four writes reached a terminal state");
  TEST_ASSERT(succeeded == 3,
              "exactly one write pays the debt and three are acknowledged normally -- "
              "under the window design all four failed, each timeout re-arming the "
              "suppression for the next");
}

// ── The debt's arithmetic: paid down, and expiring unpaid ──────────────────
// Two rules that the cascade test above cannot reach, because it lets each
// command run out its own 3 s timeout and so never puts two candidate frames
// inside one window.
//
// PAID DOWN: one owed reply costs exactly one frame. The second frame in the
// same window is this command's own answer and must be matched. Without the
// decrement the debt stands until the window lapses and eats that one too.
//
// EXPIRES UNPAID: a debt the pump never settles must not outlive its window.
// Nothing obliges the pump to send the reply it owes -- it may simply never
// answer -- and a debt kept forever would spend the next acknowledgement, then
// the next.
void test_the_reply_debt_is_paid_down_and_expires() {
  const std::vector<uint8_t> ack = with_crc({0x24, 0x05, 0xF8, 0xE7, 0x0A, 0x01, 0x00, 0x00, 0x00});

  auto owe_one_reply = [&](esphome::alpha_hwr::core::Transport &t) {
    int lost = 0;
    t.send_command(std::vector<uint8_t>(10, 0xAA), 91, 430,
                   [&](bool, const uint8_t *, size_t) { lost++; }, /*timeout_ms=*/200);
    pump(t, 3);
    mock_millis += 300;
    t.loop();
    TEST_ASSERT(lost == 1, "a command gave up, so the pump owes a reply");
  };

  {
    std::cout << "\n=== The reply debt is paid down, not left standing ===" << std::endl;
    esphome::alpha_hwr::core::Transport transport;
    transport.set_write_callback([](const uint8_t *, size_t) -> bool { return true; });
    owe_one_reply(transport);

    // Both writes have to sit inside ONE window, or the debt expires by time and
    // the decrement is never what let the second through. So this one is queued
    // with a short timeout: it gives up while the window is still open.
    std::vector<uint8_t> req = {0x27, 0x0B, 0xE7, 0xF8, 0x0A, 0x97, 91, 0x01, 0xAE, 0x00, 0x00, 0x00};
    int first = 0;
    transport.send_command(req, 0, 0,
                           [&](bool, const uint8_t *, size_t) { first++; },
                           /*timeout_ms=*/100, /*allow_register_read=*/false,
                           /*expect_short_ack=*/true);
    pump(transport, 2);
    transport.on_notification(ack.data(), ack.size());
    transport.loop();
    TEST_ASSERT(first == 0, "the first frame pays the debt rather than answering this write");
    mock_millis += 150;                  // let it give up, still inside the window
    transport.loop();

    // Second write, still well inside the original window. The debt is settled,
    // so this one gets its own answer.
    int second = 0; bool second_ok = false;
    queue_a_class10_temperature_write(transport, &second, &second_ok);
    pump(transport, 2);
    transport.on_notification(ack.data(), ack.size());
    transport.loop();
    TEST_ASSERT(second == 1 && second_ok,
                "and the next write is answered normally -- one owed reply costs one "
                "frame, not every frame until the window lapses");
  }

  {
    std::cout << "\n=== A debt the pump never settles expires ===" << std::endl;
    esphome::alpha_hwr::core::Transport transport;
    transport.set_write_callback([](const uint8_t *, size_t) -> bool { return true; });
    owe_one_reply(transport);

    // The owed reply simply never comes. Wait the window out with the link idle.
    mock_millis += esphome::alpha_hwr::core::Transport::STALE_REPLY_WINDOW_MS + 100;
    transport.loop();

    int calls = 0; bool ok = false;
    queue_a_class10_temperature_write(transport, &calls, &ok);
    pump(transport, 2);
    transport.on_notification(ack.data(), ack.size());
    transport.loop();
    TEST_ASSERT(calls == 1 && ok,
                "once the window has passed the debt is written off, and the next "
                "write is answered rather than paying for a reply that never came");
  }
}

// ── An APDU larger than a telegram is refused, not built past the buffer ────
// The size ceilings are the protocol's: a telegram is at most 259 bytes and its
// PDU at most 253 (App. Prog. Manual, "Short form technical specification").
// build_geni_packet used to test `length > 255` instead -- the widest value the
// length byte can hold -- and a frame is `length + 4` bytes, so an accepted
// length of 255 wrote 259 bytes into the 256-byte buffer send_apdu_command
// declared. Nothing built an APDU that large, which is why it survived; the
// ceiling sitting above the buffer is the defect either way.
//
// Driven through send_apdu_command because that is where the buffer lives. ASan
// is what makes the oversize case an assertion about memory and not just about
// a return value -- the suite runs under it in CI.
void test_an_oversize_apdu_is_refused() {
  std::cout << "\n=== An APDU too large for a telegram is refused ===" << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  int writes = 0;
  transport.set_write_callback([&](const uint8_t *, size_t) -> bool { writes++; return true; });

  // One byte past what a PDU may carry: DA + SA + APDU must fit MAX_PDU_LEN.
  std::vector<uint8_t> too_big(esphome::alpha_hwr::protocol::MAX_PDU_LEN - 1, 0xAA);
  int cb_calls = 0;
  bool cb_success = true;
  transport.send_apdu_command(too_big.data(), too_big.size(), 0, 0,
                              [&](bool ok, const uint8_t *, size_t) { cb_calls++; cb_success = ok; });
  TEST_ASSERT(cb_calls == 1 && !cb_success,
              "the caller is told immediately rather than waiting out a timeout for a "
              "command that was never queued");
  transport.loop();
  TEST_ASSERT(writes == 0, "and nothing reached the wire");

  // The largest APDU that IS legal still builds, and its telegram is longer than
  // the 256 bytes the buffer used to be -- which is the case that overflowed.
  std::vector<uint8_t> biggest(esphome::alpha_hwr::protocol::MAX_PDU_LEN - 2, 0xBB);
  cb_calls = 0;
  transport.send_apdu_command(biggest.data(), biggest.size(), 0, 0,
                              [&](bool, const uint8_t *, size_t) { cb_calls++; });
  TEST_ASSERT(cb_calls == 0, "the largest legal APDU is accepted, not refused");
  for (int i = 0; i < 40; i++) { mock_millis += 20; transport.loop(); }
  TEST_ASSERT(writes > 0, "and it is sent");
}

void test_a_refused_class10_write_reports_failure() {
  std::cout << "\n=== A refused Class 10 write reports failure ===" << std::endl;

  struct Case {
    uint8_t head;
    uint8_t next;      // ignored when the head declares no payload
    bool has_payload;  // false -> the 8-byte, zero-payload frame shape
    bool expect_success;
    const char *what;
  };
  const Case cases[] = {
      {0x01, 0x00, true, true, "0x01 (ack ok) with Class 10 ack OK is accepted"},
      // The SECOND acknowledge. A Class 10 reply carries its own status byte
      // after the head, and the head can say ok while it does not. Named by the
      // GO app's decoder (GeniAPDU.CLASS10_ACK_BUSY / _OPERATION_FAILED) and
      // present in the captures with exactly those values: of 459 short Class 10
      // replies, 26 are busy and 13 are operation-failed, all with head ack ok.
      // Reading the head alone called every one of them a successful write.
      {0x01, 0x02, true, false,
       "Class 10 busy is not success, though the APDU head says ok"},
      {0x01, 0x04, true, false,
       "Class 10 operation-failed is not success, though the APDU head says ok"},
      {0x01, 0x07, true, false,
       "and an unknown Class 10 status is not success either -- only 0 is"},
      // No payload, so no Class 10 status byte: the head is the whole answer.
      {0x00, 0x00, false, true, "a zero-length Class 10 reply with ack ok is accepted"},
      // A head that DECLARES one payload byte on an eight-byte frame -- the byte
      // it points at is the CRC high byte, not a status. The bound has to be the
      // real short-ACK length of 9; at `len >= 7` this frame's CRC decides the
      // write's verdict.
      {0x01, 0x00, false, true,
       "an 8-byte frame whose head declares a payload does not have its CRC read "
       "as the Class 10 status"},
      {0x81, 0x00, true, false,
       "0x81 with a following 0x00 -- the exact frame captured on hardware -- is a "
       "refusal, where the old reading called it accepted"},
      {0x81, 0x2F, true, false, "0x81 with any other item ID is a refusal too"},
      {0xC1, 0x2F, true, false, "0xC1 (Illegal Operation) is a refusal, not a timeout"},
      // Unknown Class declares length 0, so this frame is 8 bytes and there is
      // no item ID byte. The first cut of this test asserted 0x41 instead --
      // a length-1 Unknown Class, which App C.17's format table says does not
      // occur -- so it pinned a frame that cannot exist while the one that can
      // went unmatched and died by 3 s timeout. Caught by an adversarial review
      // pass, not by the suite.
      {0x40, 0x00, false, false,
       "0x40 (Unknown Class) is a refusal, and it arrives with no payload at all"},
  };

  for (const Case &c : cases) {
    esphome::alpha_hwr::core::Transport transport;
    transport.set_write_callback([](const uint8_t *, size_t) -> bool { return true; });

    int cb_calls = 0;
    bool cb_success = true;
    queue_a_class10_temperature_write(transport, &cb_calls, &cb_success);

    std::vector<uint8_t> reply =
        c.has_payload
            ? with_crc({0x24, 0x05, 0xF8, 0xE7, 0x0A, c.head, c.next, 0x00, 0x00})
            : with_crc({0x24, 0x04, 0xF8, 0xE7, 0x0A, c.head, 0x00, 0x00});
    transport.on_notification(reply.data(), reply.size());

    TEST_ASSERT(cb_calls == 1, c.what);
    TEST_ASSERT(cb_success == c.expect_success, "  ...with the right verdict");
  }
}

// The DHW config write reaches the same branch through a different address
// alternative (OpSpec 0x8F, Obj 91 Sub 421). Covered because a guard with four
// address shapes is exactly where one gets broken without anyone noticing --
// and one of the four is already dead: the only emitter of the Sub 5600 /
// Obj 0601 shape sends without a callback, so it never reaches this branch.
void test_the_dhw_config_write_shape_also_reports_a_refusal() {
  std::cout << "\n=== The DHW config shape reaches the same branch ===" << std::endl;

  esphome::alpha_hwr::core::Transport transport;
  transport.set_write_callback([](const uint8_t *, size_t) -> bool { return true; });

  int cb_calls = 0;
  bool cb_success = true;
  std::vector<uint8_t> req = {0x27, 0x0B, 0xE7, 0xF8, 0x0A, 0x8F, 91, 0x01, 0xA5, 0x00, 0x00, 0x00};
  transport.send_command(req, 0, 0, [&](bool ok, const uint8_t *, size_t) {
    cb_calls++;
    cb_success = ok;
  }, /*timeout_ms=*/3000, /*allow_register_read=*/false, /*expect_short_ack=*/true);
  mock_millis += 50;
  transport.loop();

  std::vector<uint8_t> reply = with_crc({0x24, 0x05, 0xF8, 0xE7, 0x0A, 0xC1, 0x2F, 0x00, 0x00});
  transport.on_notification(reply.data(), reply.size());

  TEST_ASSERT(cb_calls == 1, "The 0x8F shape is matched by the short-ACK branch too");
  TEST_ASSERT(!cb_success, "...and its refusal is reported as one");
}

// The refusal must also free the transport, or one refused write wedges every
// command behind it -- which would be a far worse bug than the misreport.
void test_a_refused_write_still_frees_the_transport() {
  std::cout << "\n=== A refused write frees the transport ===" << std::endl;

  esphome::alpha_hwr::core::Transport transport;
  transport.set_write_callback([](const uint8_t *, size_t) -> bool { return true; });

  int first_calls = 0;
  bool first_success = true;
  queue_a_class10_temperature_write(transport, &first_calls, &first_success);

  std::vector<uint8_t> refusal = with_crc({0x24, 0x05, 0xF8, 0xE7, 0x0A, 0x81, 0x00, 0x00, 0x00});
  transport.on_notification(refusal.data(), refusal.size());
  TEST_ASSERT(first_calls == 1 && !first_success, "The refused write reported failure");

  int second_calls = 0;
  bool second_success = false;
  queue_a_class10_temperature_write(transport, &second_calls, &second_success);
  std::vector<uint8_t> ok = with_crc({0x24, 0x05, 0xF8, 0xE7, 0x0A, 0x01, 0x00, 0x00, 0x00});
  transport.on_notification(ok.data(), ok.size());

  TEST_ASSERT(second_calls == 1 && second_success,
              "...and the next command still goes out and is answered, so the "
              "queue advanced rather than wedging on the refusal");
}


// ── Every awaited Class 10 SET gets its own acknowledgement ────────────────
// Issue #253. Until this change the short-ACK branch carried a list of five
// address shapes, and a write not on the list could not be answered no matter
// what it declared. That list was the reason four Class 10 sends were left
// fire-and-forget: giving one a callback meant remembering to add a row, and a
// row added for a send that had no callback (Obj 0601) sat unused while a row
// for a shape nothing builds (`01 AE 00 5B`) sat dead.
//
// The list is gone. What admits a frame now is `expect_short_ack` -- the
// caller's declaration that it is awaiting exactly this reply -- which is sound
// because the reply carries no way to tell the writes apart anyway: every SET
// this pump answers, it answers with the same nine bytes. The specification
// says the same thing in advance, "the SET operation never returns anything but
// the APDU Head" (App. Prog. Manual fig 3.5 note 1).
//
// So this walks every Class 10 SET the component sends, in its real on-the-wire
// shape, and requires each to be matched. A frame here that stops matching is a
// send whose acknowledgement has gone unclaimed -- which is the whole of #248.
struct AwaitedSet {
  const char *what;
  std::vector<uint8_t> apdu_head_and_address;  // class, head, then the address bytes
};

void test_every_awaited_class10_set_is_answered() {
  std::cout << "\n=== Every awaited Class 10 SET consumes its own ACK ===" << std::endl;

  // Head byte and leading address bytes exactly as each service builds them.
  const std::vector<AwaitedSet> sends = {
      {"temperature range (Obj 91 Sub 430)",      {0x0A, 0x97, 0x5B, 0x01, 0xAE, 0x03}},
      {"DHW config (Obj 91 Sub 421)",             {0x0A, 0x8F, 0x5B, 0x01, 0xA5, 0x03}},
      {"mode write (Sub 5600 Obj 0A01)",          {0x0A, 0x90, 0x56, 0x00, 0x0A, 0x01}},
      {"control request (Sub 5600 Obj 0601)",     {0x0A, 0x90, 0x56, 0x00, 0x06, 0x01}},
      {"setpoint register write (Sub 13 Obj 86)", {0x0A, 0x88, 0x00, 0x0D, 0x00, 0x56}},
      {"clock write (Obj 94 Sub 100)",            {0x0A, 0x94, 0x5E, 0x00, 0x64, 0x01}},
      {"schedule commit (Obj 84 Sub 1)",          {0x0A, 0x93, 0x54, 0x00, 0x01, 0x00}},
      {"schedule layer write (Obj 84 Sub 1000)",  {0x0A, 0xB3, 0x54, 0x03, 0xE8, 0x00}},
  };

  // The one frame the pump answers all of them with, byte for byte as captured.
  const std::vector<uint8_t> ack = {0x24, 0x05, 0xF8, 0xE7, 0x0A, 0x01, 0x00, 0xAE, 0xA2};

  for (const auto &s : sends) {
    esphome::alpha_hwr::core::Transport transport;
    transport.set_write_callback([](const uint8_t *, size_t) -> bool { return true; });

    std::vector<uint8_t> req = {0x27, 0x0B, 0xE7, 0xF8};
    req.insert(req.end(), s.apdu_head_and_address.begin(), s.apdu_head_and_address.end());
    req.push_back(0x00);
    req.push_back(0x00);

    int calls = 0;
    bool ok = false;
    transport.send_command(req, 0, 0,
                           [&](bool success, const uint8_t *, size_t) { calls++; ok = success; },
                           esphome::alpha_hwr::core::Transport::SET_ACK_TIMEOUT_MS,
                           /*allow_register_read=*/false, /*expect_short_ack=*/true,
                           /*quiet_timeout=*/true);
    mock_millis += 50;
    transport.loop();
    transport.on_notification(ack.data(), ack.size());
    transport.loop();

    TEST_ASSERT(calls == 1 && ok, std::string("answered: ") + s.what);
  }
}

// The other half of the same rule, and the reason replacing the address list
// with a declaration is a NARROWING rather than a loosening.
//
// A Class 10 SET that has not declared it is awaiting a short ACK does not get
// handed one. Under the old address list this depended on which write it was;
// now it depends on what the caller asked for, which is the thing the caller
// actually knows. Without this the gate could be deleted and the suite would
// not notice -- every other test here declares the flag.
void test_an_undeclared_class10_set_is_not_handed_a_short_ack() {
  std::cout << "\n=== A Class 10 SET that did not ask for a short ACK is not given one ==="
            << std::endl;

  esphome::alpha_hwr::core::Transport transport;
  transport.set_write_callback([](const uint8_t *, size_t) -> bool { return true; });

  // The temperature-range write's own shape -- one that WOULD match if it had
  // declared -- so what separates this case is the declaration and nothing else.
  std::vector<uint8_t> req = {0x27, 0x0B, 0xE7, 0xF8, 0x0A, 0x97, 0x5B, 0x01, 0xAE, 0x03, 0x00, 0x00};
  int calls = 0;
  transport.send_command(req, 0, 0,
                         [&](bool, const uint8_t *, size_t) { calls++; },
                         /*timeout_ms=*/3000, /*allow_register_read=*/false,
                         /*expect_short_ack=*/false);
  mock_millis += 50;
  transport.loop();

  const std::vector<uint8_t> ack = {0x24, 0x05, 0xF8, 0xE7, 0x0A, 0x01, 0x00, 0xAE, 0xA2};
  transport.on_notification(ack.data(), ack.size());
  transport.loop();

  TEST_ASSERT(calls == 0,
              "the frame fell through to the packet callback rather than settling a "
              "command that never said it was waiting for one");
}

int main() {
  test_reassembly_continuation_0x24();
  test_reassembly_continuation_0x27();
  test_reassembly_stale_partial_recovers();
  test_trailing_bytes_are_trimmed();
  test_mode_read_matches_without_fallback();
  test_bad_crc_frame_is_dropped();
  test_bad_crc_cannot_answer_a_command();
  test_length_collision_does_not_veto_a_type_match();

  std::cout << "===========================================================" << std::endl;
  std::cout << "  Transport FSM Test Suite" << std::endl;
  std::cout << "===========================================================" << std::endl;
  
  test_transport_chunking();
  test_send_failure_fails_the_command_and_frees_the_transport();
  test_send_failure_midway_through_a_chunked_packet();
  test_partial_write_holds_off_so_the_peer_can_resync();
  test_first_chunk_failure_does_not_hold_off();
  test_missing_write_callback_drops_the_command();
  test_a_command_honours_its_own_timeout_not_the_default();
  test_an_inbound_frame_never_starts_with_the_request_delimiter();
  test_a_frame_start_declaring_less_than_a_telegram_is_refused();
  test_a_maximum_length_legal_frame_is_not_read_as_an_overflow();
  test_a_lone_frame_start_byte_does_not_swallow_what_follows();
  test_trailing_bytes_do_not_overflow_a_frame_that_is_already_complete();
  test_an_inbound_overflow_does_not_cancel_a_command_in_flight();
  test_an_inbound_overflow_keeps_the_peer_resync_hold();
  test_an_inbound_overflow_keeps_the_reply_debt();
  test_reset_fails_a_pending_command_instead_of_dropping_it();
  test_reset_fails_a_command_that_never_went_out();
  test_reset_takes_the_commands_its_own_callbacks_queue();
  test_reset_from_inside_an_abandoned_callback_does_not_double_fire();
  test_a_callback_that_queues_then_resets_does_not_recurse_per_step();
  test_a_success_callback_that_resets_does_not_re_enter_itself();
  test_a_timeout_callback_that_resets_does_not_re_enter_itself();
  test_a_chain_that_never_stops_re_sending_hits_the_cap();
  test_a_late_reply_costs_one_match_and_no_more();
  test_the_reply_debt_is_paid_down_and_expires();
  test_an_oversize_apdu_is_refused();
  test_a_refused_class10_write_reports_failure();
  test_the_dhw_config_write_shape_also_reports_a_refusal();
  test_a_refused_write_still_frees_the_transport();
  test_every_awaited_class10_set_is_answered();
  test_an_undeclared_class10_set_is_not_handed_a_short_ack();
  
  std::cout << "\n===========================================================" << std::endl;
  std::cout << "  Test Results" << std::endl;
  std::cout << "===========================================================" << std::endl;
  std::cout << "Tests passed: " << tests_passed << std::endl;
  std::cout << "Tests failed: " << tests_failed << std::endl;

  return tests_failed == 0 ? 0 : 1;
}

