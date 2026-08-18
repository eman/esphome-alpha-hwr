#include <iostream>
#include <vector>
#include <cstdint>
#include "fixture_crc.h"
#include "../components/alpha_hwr/transport.h"

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
// tests/test_auth.cpp until the opening sequence was removed (issue #174), and
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

// ── reset() drops queued callbacks without invoking them ────────────────────
// Not a desirable property -- a hazard, pinned so it stays visible. reset() is
// reachable on a live link from one corrupt inbound fragment, and any service
// with a read in flight when it happens waits forever: DeviceInfoService,
// ScheduleService and TelemetryService all queue commands with callbacks.
//
// The opening sequence carried a whole-sequence backstop for exactly this, and
// tests/test_auth.cpp was the only place the hazard was demonstrated. Both are
// gone (issue #174), so this case exists to keep the hazard in the tree.
//
// The better repair is to fire each queued callback with failure from reset()
// itself, which would let this assertion be inverted. Nobody has done it.
void test_reset_abandons_a_pending_command_without_telling_it() {
  std::cout << "\n=== reset() abandons a pending command silently ===" << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  transport.set_write_callback([](const uint8_t *, size_t) -> bool { return true; });

  int cb_calls = 0;
  transport.send_command(std::vector<uint8_t>(10, 0xAA), 0, 0,
                         [&](bool, const uint8_t *, size_t) { cb_calls++; },
                         /*timeout_ms=*/1000);

  mock_millis += 50;
  transport.loop();   // in flight, awaiting a response

  transport.reset();

  mock_millis += 10000;   // ten times the command's own window
  transport.loop();

  TEST_ASSERT(cb_calls == 0,
              "The callback never fires -- reset() dropped the command, and the "
              "timeout that would have failed it went with the queue entry. A "
              "caller waiting on this reply waits forever.");
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
  // OpSpec 0x97 with the Obj 91 / Sub 430 address, which is one of the shapes
  // the short-ACK branch is guarded on.
  std::vector<uint8_t> req = {0x27, 0x0B, 0xE7, 0xF8, 0x0A, 0x97, 91, 0x01, 0xAE, 0x00, 0x00, 0x00};
  t.send_command(req, 0, 0,
                 [cb_calls, cb_success](bool ok, const uint8_t *, size_t) {
                   (*cb_calls)++;
                   *cb_success = ok;
                 });
  mock_millis += 50;
  t.loop();
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
      {0x01, 0x00, true, true, "0x01 (ack ok) is still accepted"},
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
  });
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
  test_reset_abandons_a_pending_command_without_telling_it();
  test_a_refused_class10_write_reports_failure();
  test_the_dhw_config_write_shape_also_reports_a_refusal();
  test_a_refused_write_still_frees_the_transport();
  
  std::cout << "\n===========================================================" << std::endl;
  std::cout << "  Test Results" << std::endl;
  std::cout << "===========================================================" << std::endl;
  std::cout << "Tests passed: " << tests_passed << std::endl;
  std::cout << "Tests failed: " << tests_failed << std::endl;

  return tests_failed == 0 ? 0 : 1;
}

