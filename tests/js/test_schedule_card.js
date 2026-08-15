/**
 * Host tests for homeassistant/www/alpha-hwr-schedule-card.js.
 *
 * The card is 1800 lines that had no automated check beyond `node --check`, and
 * it has already shipped broken twice for want of one: every service call
 * omitted the required `op_id`, and the single-event regex could not match the
 * format the firmware emits. Both were silent.
 *
 * No dependencies and no npm install, matching the C++ suite's stance: the
 * check has to run everywhere, forever, without a lockfile to rot. The card
 * touches a small, fixed set of DOM APIs, so they are stubbed below rather than
 * pulled in as jsdom.
 *
 * Run: node tests/js/test_schedule_card.js
 */

'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');

/* ─── Assertions ─── */

let assertions = 0;
let failures = 0;
let currentTest = '';

// Bound up front: quietly() swaps console.error out from under the card, and a
// failure reported through the muted one would be a test that cannot fail.
const report = console.error.bind(console);

function assert(cond, what) {
  assertions++;
  if (!cond) {
    failures++;
    report(`  FAIL  ${currentTest}: ${what}`);
  }
}

function assertEqual(actual, expected, what) {
  assert(actual === expected, `${what} (expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)})`);
}

async function test(name, fn) {
  currentTest = name;
  const before = failures;
  // Timers are global and their deadlines absolute, so a test that leaves one
  // armed would otherwise be read as the next test's backstop.
  timers.clear();
  fakeNow = 0;
  try {
    await fn();
  } catch (err) {
    failures++;
    report(`  FAIL  ${name}: threw ${err && err.stack ? err.stack : err}`);
  }
  console.log(`  ${failures === before ? 'ok  ' : 'FAIL'}  ${name}`);
}

/* ─── DOM stub ─── */

/** A shadow root that swallows markup but keeps it inspectable.
 *
 * Element queries return nothing, which is what the card's own event wiring
 * tolerates (`querySelectorAll(...).forEach`) -- these tests drive the card's
 * methods directly rather than through synthesised clicks, so the markup only
 * has to be *produced* without throwing, and readable afterwards for the
 * escaping test.
 */
function makeShadowRoot() {
  return {
    innerHTML: '',
    querySelector() { return null; },
    querySelectorAll() { return []; },
  };
}

class StubHTMLElement {
  attachShadow() {
    this.shadowRoot = makeShadowRoot();
    return this.shadowRoot;
  }
}

/* ─── Fake timers ─── */

const timers = new Map();
let nextTimerId = 1;
let fakeNow = 0;

function fakeSetTimeout(fn, ms) {
  const id = nextTimerId++;
  timers.set(id, { fn, at: fakeNow + (ms || 0) });
  return id;
}

function fakeClearTimeout(id) { timers.delete(id); }

/** Advance the fake clock, firing anything due, in time order. */
function advance(ms) {
  fakeNow += ms;
  const due = [...timers.entries()]
    .filter(([, t]) => t.at <= fakeNow)
    .sort((a, b) => a[1].at - b[1].at);
  for (const [id, t] of due) {
    timers.delete(id);
    t.fn();
  }
}

/** The delay a pending timer was armed with, for asserting the backstop. */
function soonestTimerDelay() {
  let soonest = null;
  for (const t of timers.values()) {
    if (soonest === null || t.at < soonest) soonest = t.at;
  }
  return soonest === null ? null : soonest - fakeNow;
}

/* ─── Load the card ─── */

const CARD_PATH = path.join(__dirname, '..', '..', 'homeassistant', 'www',
                            'alpha-hwr-schedule-card.js');

const registry = new Map();
const sandbox = {
  HTMLElement: StubHTMLElement,
  customElements: { define: (name, cls) => registry.set(name, cls) },
  window: {},
  document: {
    createElement: () => ({ style: {}, classList: { add() {}, remove() {} }, remove() {} }),
    body: { appendChild() {} },
    addEventListener() {},
    removeEventListener() {},
  },
  console,
  setTimeout: fakeSetTimeout,
  clearTimeout: fakeClearTimeout,
  setInterval: () => 0,
  clearInterval: () => {},
  Promise,
  Date,
  Math,
  JSON,
};
sandbox.globalThis = sandbox;

vm.createContext(sandbox);
vm.runInContext(fs.readFileSync(CARD_PATH, 'utf8'), sandbox, { filename: CARD_PATH });

const CardClass = registry.get('alpha-hwr-schedule-card');
if (!CardClass) throw new Error('card did not register alpha-hwr-schedule-card');

/* ─── Harness ─── */

/** A card wired to a fake hass, with every service call and event recorded. */
function makeCard() {
  const calls = [];
  let settleHandler = null;
  let unsubscribed = 0;
  let rejectNext = null;

  const card = new CardClass();
  card.setConfig({ device: 'hwr_pump' });

  const hass = {
    states: {},
    connection: {
      subscribeEvents(cb, type) {
        settleHandler = { cb, type };
        return Promise.resolve(() => { unsubscribed++; });
      },
    },
    callService(domain, service, data) {
      calls.push({ domain, service, data });
      if (rejectNext && rejectNext === service) {
        rejectNext = null;
        return Promise.reject(new Error('service not found'));
      }
      return Promise.resolve();
    },
  };

  card.hass = hass;

  return {
    card,
    calls,
    get unsubscribed() { return unsubscribed; },
    get subscribedType() { return settleHandler && settleHandler.type; },
    rejectOn(service) { rejectNext = service; },
    /** The op_ids the card issued, oldest first. */
    opIds() { return calls.filter(c => c.data && c.data.op_id).map(c => c.data.op_id); },
    /** Deliver a settle event the way HA would. */
    settle(opId, status, detail) {
      settleHandler.cb({ data: { op_id: opId, status, detail: detail || '', node: 'hwr_pump' } });
    },
    countOf(service) {
      return calls.filter(c => c.service === `hwr_pump_${service}`).length;
    },
  };
}

/** Wait for pending microtasks (promise .catch handlers) to run. */
const flush = () => new Promise(resolve => process.nextTick(resolve));

/** Run fn with console.error muted -- for the paths that log on purpose. */
async function quietly(fn) {
  const real = console.error;
  console.error = () => {};
  try { await fn(); } finally { console.error = real; }
}

/* ─── Tests ─── */

async function main() {
  console.log('==========================================');
  console.log('  Lovelace card tests');
  console.log('==========================================');

  // -------------------------------------------------------------------------
  // Write confirmation. The card used to drop every edit the instant it fired
  // the write, then re-read the device 3 s later -- well inside the 20 s
  // watchdog a schedule write carries. A failed write and a successful one
  // were indistinguishable, and both ended with the user's edit gone.
  // -------------------------------------------------------------------------

  await test('an accepted write retires its edit', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();

    assertEqual(h.countOf('set_schedule_entry'), 1, 'one write issued');
    assertEqual(h.card._pendingChanges.size, 1, 'edit is held while the write is out');

    h.settle(h.opIds()[0], 'accepted');
    assertEqual(h.card._pendingChanges.size, 0, 'edit retired on confirmation');
    assertEqual(h.card._writeErrors.length, 0, 'no error reported');
    assertEqual(h.countOf('refresh_schedule'), 1, 'refreshed once the batch drained');
  });

  await test('a rejected write keeps its edit and says so', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('2,1', [420, 500]);
    h.card._saveChanges();
    h.settle(h.opIds()[0], 'rejected', 'pump not connected/synchronized');

    assertEqual(h.card._pendingChanges.size, 1, 'the edit survives a failed write');
    assertEqual(h.card._writeErrors.length, 1, 'the failure is reported');
    assertEqual(h.card._writeErrors[0].status, 'rejected', 'status carried through');
    assert(h.card.shadowRoot.innerHTML.includes('pump not connected'),
           'the detail reaches the rendered card');
  });

  await test('a clamped write counts as accepted', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();
    h.settle(h.opIds()[0], 'clamped', 'end clamped to 23:59');

    assertEqual(h.card._pendingChanges.size, 0, 'the device took the write');
    assertEqual(h.card._writeErrors.length, 0, 'a clamp is not a failure');
  });

  await test('the refresh waits for the last write in the batch', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._pendingChanges.set('1,0', [400, 500]);
    h.card._pendingChanges.set('2,3', null);
    h.card._saveChanges();

    assertEqual(h.countOf('set_schedule_entry'), 2, 'two sets');
    assertEqual(h.countOf('clear_schedule_entry'), 1, 'one clear');

    const ops = h.opIds();
    h.settle(ops[0], 'accepted');
    h.settle(ops[1], 'accepted');
    assertEqual(h.countOf('refresh_schedule'), 0, 'no read while a write is still out');

    h.settle(ops[2], 'accepted');
    assertEqual(h.countOf('refresh_schedule'), 1, 'exactly one read, after the last settle');
  });

  await test('an edit made during the write outlives its confirmation', async () => {
    // The user drags the same block again before the first write lands. The
    // confirmation belongs to the *old* value, so retiring the cell wholesale
    // would silently throw away the newer intent.
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();
    h.card._pendingChanges.set('0,0', [300, 420]); // re-edited, in flight

    h.settle(h.opIds()[0], 'accepted');
    assertEqual(h.card._pendingChanges.size, 1, 'the newer edit is still pending');
    const held = h.card._pendingChanges.get('0,0');
    assertEqual(held[0], 300, 'and it is the newer value, not the confirmed one');
  });

  await test('a settle for an op the card did not issue is ignored', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();

    h.settle('someone-elses-op', 'accepted');
    assertEqual(h.card._pendingChanges.size, 1, 'our edit is untouched');
    assertEqual(h.card._inFlight.size, 1, 'our write is still outstanding');
    assertEqual(h.countOf('refresh_schedule'), 0, 'and nothing was refreshed');
  });

  await test('saving again is refused while writes are outstanding', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();
    h.card._saveChanges();

    assertEqual(h.countOf('set_schedule_entry'), 1, 'the second save issued nothing');
  });

  await test('discard is refused while writes are outstanding', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();
    h.card._discardChanges();

    assertEqual(h.card._pendingChanges.size, 1,
                'the edit stays until its own write settles');
  });

  // -------------------------------------------------------------------------
  // The backstop. A settle event can genuinely never arrive -- HA restart,
  // websocket reconnect, node reboot -- and edits pinned on screen forever
  // would be its own bug.
  // -------------------------------------------------------------------------

  await test('the backstop is armed from the firmware watchdog, per write', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._pendingChanges.set('1,0', [360, 480]);
    h.card._saveChanges();

    // 2 x WATCHDOG_SCHED_ENTRY_MS + SETTLE_MARGIN_MS. Writes queue one at a
    // time, so a batch's ceiling is per-write x batch size.
    assertEqual(soonestTimerDelay(), 2 * 20000 + 5000, 'delay scales with the batch');
  });

  await test('an unanswered write is released by the backstop', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();

    advance(20000 + 5000 - 1);
    assertEqual(h.card._inFlight.size, 1, 'still waiting just before the deadline');

    advance(1);
    assertEqual(h.card._inFlight.size, 0, 'released at the deadline');
    assertEqual(h.card._writeErrors.length, 1, 'and reported rather than silently dropped');
    assertEqual(h.card._writeErrors[0].status, 'no confirmation', 'named for what happened');
    assertEqual(h.card._pendingChanges.size, 1, 'the unconfirmed edit is kept');
    assertEqual(h.countOf('refresh_schedule'), 1, 'device state is re-read');
  });

  await test('the backstop is disarmed once the batch settles', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();
    h.settle(h.opIds()[0], 'accepted');

    assertEqual(soonestTimerDelay(), null, 'no timer left armed');
    advance(60000);
    assertEqual(h.countOf('refresh_schedule'), 1, 'and no second refresh fires later');
  });

  await test('a call Home Assistant itself rejects is reported', async () => {
    // No settle event ever follows a call HA refuses, so the in-flight entry
    // would otherwise sit until the backstop and report the wrong reason.
    const h = makeCard();
    h.rejectOn('hwr_pump_set_schedule_entry');
    h.card._pendingChanges.set('0,0', [360, 480]);
    await quietly(async () => {
      h.card._saveChanges();
      await flush();
    });

    assertEqual(h.card._inFlight.size, 0, 'the write is not left outstanding');
    assertEqual(h.card._writeErrors.length, 1, 'the rejection is surfaced');
    assertEqual(h.card._writeErrors[0].status, 'call rejected', 'named for what happened');
    assertEqual(h.card._pendingChanges.size, 1, 'the edit is kept for a retry');
  });

  // -------------------------------------------------------------------------
  // Single events. Same shape, and worse numbers: a 3 s refresh against a 60 s
  // watchdog, plus an optimistic local delete that made a failed clear look
  // like a success for the better part of a minute.
  // -------------------------------------------------------------------------

  await test('a cleared event is held, not removed on faith', async () => {
    const h = makeCard();
    h.card._singleEvents = [{ slot: 2, begin: new Date(), end: new Date(), action: 'run' }];
    h.card._clearSingleEvent(2);

    assertEqual(h.countOf('clear_single_event'), 1, 'the clear was issued');
    assertEqual(h.card._singleEvents.length, 1, 'the row is still shown while unconfirmed');
    assert(h.card._slotInFlight(2), 'and marked as in flight');

    h.settle(h.opIds()[0], 'accepted');
    assertEqual(h.countOf('refresh_single_events'), 1, 'the event list is re-read on settle');
    assert(!h.card._slotInFlight(2), 'and the in-flight mark is cleared');
  });

  await test('a failed clear reports instead of vanishing the row', async () => {
    const h = makeCard();
    h.card._singleEvents = [{ slot: 2, begin: new Date(), end: new Date(), action: 'run' }];
    h.card._clearSingleEvent(2);
    h.settle(h.opIds()[0], 'timeout', 'no response');

    assertEqual(h.card._singleEvents.length, 1, 'the event is still listed');
    assertEqual(h.card._writeErrors.length, 1, 'and the failure is visible');
  });

  await test('a single-event write waits on its own 60 s watchdog', async () => {
    const h = makeCard();
    h.card._scheduleQuickRun(30);
    assertEqual(h.countOf('set_single_event'), 1, 'the write was issued');
    assertEqual(soonestTimerDelay(), 60000 + 5000, 'backstop matches the firmware budget');
  });

  // -------------------------------------------------------------------------
  // Regression guards for bugs this card has already shipped.
  // -------------------------------------------------------------------------

  await test('the event parser matches the format the firmware emits', async () => {
    // format_single_events_display appends " (run)"/" (off)"; the card's regex
    // was anchored on the end time, so every line failed to match and the
    // Quick Run list rendered empty -- indistinguishable from "no events".
    const h = makeCard();
    h.card._parseSingleEvents(
      '[0] 2026-08-15 06:00 - 07:30 (run)\n[3] 2026-08-15 22:00 - 23:30 (off)');

    assertEqual(h.card._singleEvents.length, 2, 'both lines parsed');
    assertEqual(h.card._singleEvents[0].slot, 0, 'slot read');
    assertEqual(h.card._singleEvents[0].action, 'run', 'action read');
    assertEqual(h.card._singleEvents[1].action, 'off', 'vacation action read');
    assertEqual(h.card._singleEvents[0].begin.getHours(), 6, 'begin hour');
    assertEqual(h.card._singleEvents[0].end.getMinutes(), 30, 'end minute');
  });

  await test('the sentinel and unavailable states parse to no events', async () => {
    const h = makeCard();
    for (const s of ['No single events', 'unavailable', 'unknown', '']) {
      h.card._parseSingleEvents(s);
      assertEqual(h.card._singleEvents.length, 0, `"${s}" yields no events`);
    }
  });

  await test('a block at the right edge is clamped below hour 24', async () => {
    // api_bridge.cpp rejects hour > 23, so end === 1440 could never be saved.
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [1380, 1440]);
    h.card._saveChanges();

    const write = h.calls.find(c => c.service === 'hwr_pump_set_schedule_entry');
    assertEqual(write.data.data, '0,0,23,0,23,59', 'serialised as 23:59, not 24:00');
  });

  await test('every service call carries an op_id', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();
    h.settle(h.opIds()[0], 'accepted');
    h.card._scheduleQuickRun(30);

    assert(h.calls.length >= 3, 'several calls were made');
    for (const c of h.calls) {
      assert(c.data && typeof c.data.op_id === 'string' && c.data.op_id.length > 0,
             `${c.service} carries an op_id`);
    }
    const ids = h.opIds();
    assertEqual(new Set(ids).size, ids.length, 'op_ids are unique');
  });

  // -------------------------------------------------------------------------
  // Trust boundary and lifetime.
  // -------------------------------------------------------------------------

  await test('a detail string from the device cannot inject markup', async () => {
    // `detail` echoes the failed request ("parse error: <payload>"), so it is
    // influenced by anyone who can call the service, and it lands in innerHTML.
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();
    h.settle(h.opIds()[0], 'invalid', 'parse error: <img src=x onerror="alert(1)">');

    const html = h.card.shadowRoot.innerHTML;
    assert(!html.includes('<img src=x'), 'the tag is not emitted raw');
    assert(!html.includes('onerror="alert(1)"'), 'nor is the handler');
    assert(html.includes('&lt;img'), 'it is escaped and still readable');
  });

  await test('the card subscribes once and releases on teardown', async () => {
    const h = makeCard();
    assertEqual(h.subscribedType, 'esphome.alpha_hwr_write_settled', 'subscribed to the right event');

    const sub = h.card._settleSub;
    h.card.hass = h.card._hass; // a second hass update must not re-subscribe
    assert(h.card._settleSub === sub, 'the subscription is not duplicated');

    h.card.disconnectedCallback();
    await flush();
    assertEqual(h.unsubscribed, 1, 'unsubscribed exactly once');
    assert(h.card._settleSub === null, 'and the handle is released');
  });

  await test('teardown while a write is outstanding cancels the backstop', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();
    h.card.disconnectedCallback();

    assertEqual(soonestTimerDelay(), null, 'no timer survives the card');
  });

  console.log('');
  console.log('==========================================');
  console.log(`  ${assertions} assertions, ${failures} failure(s)`);
  console.log('==========================================');
  if (failures > 0) process.exit(1);
}

main().catch(err => {
  console.error(err);
  process.exit(1);
});
