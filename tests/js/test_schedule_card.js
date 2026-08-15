/**
 * Host tests for homeassistant/www/alpha-hwr-schedule-card.js.
 *
 * The card is ~2000 lines that had no automated check beyond `node --check`,
 * and it has already shipped broken twice for want of one: every service call
 * omitted the required `op_id`, and the single-event regex could not match the
 * format the firmware emits. Both were silent.
 *
 * No dependencies and no npm install, matching the C++ suite's stance: the
 * check has to run everywhere, forever, without a lockfile to rot. The card
 * touches a small, fixed set of DOM APIs, so they are stubbed below rather than
 * pulled in as jsdom.
 *
 * **The harness renders the whole card on purpose.** An earlier version left
 * `_showQuickRun` false and `hass.states` empty, so the Quick Run panel
 * returned '' and the grid took its empty branch in every test -- the entire
 * single-event UI could be deleted outright with the suite still green. A stub
 * that never reaches the code is indistinguishable from no test at all, so
 * `makeCard()` seeds real layer states and opens the panel.
 *
 * Run: node tests/js/test_schedule_card.js   (or: make test-js)
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

function assertIncludes(haystack, needle, what) {
  assert(String(haystack).includes(needle), `${what} (missing ${JSON.stringify(needle)})`);
}

function assertExcludes(haystack, needle, what) {
  assert(!String(haystack).includes(needle), `${what} (unexpectedly contains ${JSON.stringify(needle)})`);
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
 * Element queries return nothing, which the card's own event wiring tolerates
 * (`querySelectorAll(...).forEach`). These tests drive the card's methods
 * directly rather than through synthesised clicks, so the markup has to be
 * *produced* without throwing and is then asserted on as a string -- which is
 * how the button wiring (`data-action`, `data-slot`, `disabled`) gets covered
 * despite nothing being clickable here.
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

/** How many timers are outstanding -- a stray one is a leak, not a backstop. */
function timerCount() { return timers.size; }

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

/* ─── Firmware constants, restated so a drift shows up as a failure ─── */

const SCHED_ENTRY_MS = 20000;   // WATCHDOG_SCHED_ENTRY_MS
const SINGLE_EVENT_MS = 60000;  // WATCHDOG_SINGLE_EVENT_MS
const QUEUE_HEAD_MS = 150000;   // WATCHDOG_UPLOAD_MS -- worst case ahead of us
const MARGIN_MS = 5000;         // SETTLE_MARGIN_MS

const schedBackstop = (n) => n * SCHED_ENTRY_MS + QUEUE_HEAD_MS + MARGIN_MS;
const singleBackstop = (n) => n * SINGLE_EVENT_MS + QUEUE_HEAD_MS + MARGIN_MS;

/* ─── Harness ─── */

/** A card wired to a fake hass, with every service call and event recorded.
 *
 * States are seeded so `_rebuildSchedule` produces a real grid and the Quick
 * Run panel is opened, so `_render` walks the markup these tests assert on.
 */
function makeCard(opts = {}) {
  const calls = [];
  const pendingRejections = [];
  let settleHandler = null;
  let unsubscribed = 0;
  let subscribeCount = 0;
  let rejectNext = null;

  const card = new CardClass();
  card.setConfig({ device: 'hwr_pump' });

  const layer0 = JSON.stringify([[360, 480], 0, 0, 0, 0, 0, 0]);
  const empty = JSON.stringify([0, 0, 0, 0, 0, 0, 0]);

  const hass = {
    states: {
      'sensor.hwr_pump_schedule_layer_0': { state: layer0 },
      'sensor.hwr_pump_schedule_layer_1': { state: empty },
      'sensor.hwr_pump_schedule_layer_2': { state: empty },
      'sensor.hwr_pump_schedule_layer_3': { state: empty },
      'sensor.hwr_pump_schedule_layer_4': { state: empty },
      'switch.hwr_pump_schedule_enabled': { state: 'on' },
      'sensor.hwr_pump_single_events': {
        state: opts.singleEvents !== undefined
          ? opts.singleEvents
          : '[0] 2026-08-15 06:00 - 07:30 (run)',
      },
    },
    connection: {
      subscribeEvents(cb, type) {
        subscribeCount++;
        settleHandler = { cb, type };
        if (opts.subscribeFails) {
          const p = Promise.reject(new Error('no such event'));
          pendingRejections.push(p);
          return p;
        }
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
  // Open the Quick Run panel so the single-event UI actually renders. Without
  // this, _renderQuickRunPanel returns '' and every assertion about the event
  // rows would pass against a deleted implementation.
  card._showQuickRun = true;
  card._render();

  return {
    card,
    hass,
    calls,
    get unsubscribed() { return unsubscribed; },
    get subscribeCount() { return subscribeCount; },
    get subscribedType() { return settleHandler && settleHandler.type; },
    /** Swallow the rejections this fixture created on purpose. */
    settleRejections() { pendingRejections.forEach(p => p.catch(() => {})); },
    /** A settle for an op this card never issued. */
    foreignSettle(node) {
      settleHandler.cb({
        data: { op_id: 'other-card-op', status: 'accepted', detail: '', node },
      });
    },
    get html() { return card.shadowRoot.innerHTML; },
    rejectOn(service) { rejectNext = service; },
    /** The op_ids the card issued, oldest first. */
    opIds() { return calls.filter(c => c.data && c.data.op_id).map(c => c.data.op_id); },
    /** op_ids for one service only -- untracked refreshes share the counter. */
    opIdsFor(service) {
      return calls.filter(c => c.service === `hwr_pump_${service}`).map(c => c.data.op_id);
    },
    /** Deliver a settle event the way HA would. */
    settle(opId, status, detail) {
      settleHandler.cb({ data: { op_id: opId, status, detail: detail || '', node: 'hwr_pump' } });
    },
    countOf(service) {
      return calls.filter(c => c.service === `hwr_pump_${service}`).length;
    },
    payloadsOf(service) {
      return calls.filter(c => c.service === `hwr_pump_${service}`).map(c => c.data.data);
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

    h.settle(h.opIdsFor('set_schedule_entry')[0], 'accepted');
    assertEqual(h.card._pendingChanges.size, 0, 'edit retired on confirmation');
    assertEqual(h.card._writeErrors.length, 0, 'no error reported');
    assertEqual(h.countOf('refresh_schedule'), 1, 'refreshed once the batch drained');
  });

  await test('a rejected write names the edit that failed', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('2,1', [420, 500]);
    h.card._saveChanges();
    h.settle(h.opIdsFor('set_schedule_entry')[0], 'rejected', 'pump not connected/synchronized');

    assertEqual(h.card._pendingChanges.size, 1, 'the edit survives a failed write');
    assertEqual(h.card._writeErrors.length, 1, 'the failure is reported');
    assertEqual(h.card._writeErrors[0].status, 'rejected', 'status carried through');
    // Which edit failed is the entire point of a per-write report; a batch
    // that says only "rejected" tells the user nothing actionable.
    assertEqual(h.card._writeErrors[0].label, 'Wed layer 1', 'the failing cell is identified');
    assertIncludes(h.html, 'Wed layer 1', 'the label reaches the rendered card');
    assertIncludes(h.html, 'pump not connected', 'so does the device detail');
  });

  await test('a clamped write counts as accepted', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();
    h.settle(h.opIdsFor('set_schedule_entry')[0], 'clamped', 'end clamped to 23:59');

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

  await test('every failure in a batch is reported, not just the first', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._pendingChanges.set('1,0', [400, 500]);
    h.card._pendingChanges.set('2,0', [420, 540]);
    h.card._saveChanges();

    const ops = h.opIds();
    h.settle(ops[0], 'rejected', 'first');
    h.settle(ops[1], 'accepted');
    h.settle(ops[2], 'timeout', 'third');

    assertEqual(h.card._writeErrors.length, 2, 'both failures kept');
    assertEqual(h.card._pendingChanges.size, 2, 'and both edits kept for a retry');
    assertIncludes(h.html, 'Mon layer 0', 'the first failure is rendered');
    assertIncludes(h.html, 'Wed layer 0', 'and so is the second');
  });

  await test('an edit made during the write outlives its confirmation', async () => {
    // The user drags the same block again before the first write lands. The
    // confirmation belongs to the *old* array, so the check has to be identity
    // -- a value comparison would retire the cell whenever the user happened
    // to drag it back to where it started.
    const h = makeCard();
    const original = [360, 480];
    h.card._pendingChanges.set('0,0', original);
    h.card._saveChanges();
    h.card._pendingChanges.set('0,0', [360, 480]); // same value, fresh array

    h.settle(h.opIdsFor('set_schedule_entry')[0], 'accepted');
    assertEqual(h.card._pendingChanges.size, 1, 'the newer edit is still pending');
    assert(h.card._pendingChanges.get('0,0') !== original,
           'and it is the newer array, not the confirmed one');
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

  await test('saving with nothing pending does nothing at all', async () => {
    const h = makeCard();
    h.card._saveChanges();

    assertEqual(h.calls.length, 0, 'no service call');
    assertEqual(timerCount(), 0, 'and no phantom backstop armed');
  });

  await test('discard is refused while writes are outstanding', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();
    h.card._discardChanges();

    assertEqual(h.card._pendingChanges.size, 1,
                'the edit stays until its own write settles');
  });

  await test('a retry clears the previous failure report', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();
    h.settle(h.opIdsFor('set_schedule_entry')[0], 'rejected', 'pump not connected');
    assertEqual(h.card._writeErrors.length, 1, 'reported once');
    assertIncludes(h.html, 'pump not connected', 'and shown');

    h.card._saveChanges(); // retry
    assertEqual(h.card._writeErrors.length, 0, 'the stale banner is cleared on retry');
    assertExcludes(h.html, 'pump not connected', 'and gone from the card');
  });

  await test('discarding clears the failure report too', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();
    h.settle(h.opIdsFor('set_schedule_entry')[0], 'rejected', 'pump not connected');

    h.card._discardChanges();
    assertEqual(h.card._pendingChanges.size, 0, 'the edit is dropped');
    assertEqual(h.card._writeErrors.length, 0, 'and so is the error about it');
  });

  await test('a single-event action does not erase an unread schedule failure', async () => {
    // The two surfaces fail independently. A Quick Run is not a retry of a
    // schedule write, so it must not wipe the report that says which cell
    // failed and why -- the edit is still pending and the user has not read it.
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();
    h.settle(h.opIdsFor('set_schedule_entry')[0], 'rejected', 'pump not connected');

    h.card._scheduleQuickRun(30);
    assertEqual(h.card._writeErrors.length, 1, 'the schedule failure survives');
    assertEqual(h.card._writeErrors[0].surface, 'schedule', 'and is still attributed');
    assertIncludes(h.html, 'pump not connected', 'and is still on screen');
  });

  // -------------------------------------------------------------------------
  // The backstop. A settle event can genuinely never arrive -- HA restart,
  // websocket reconnect, node reboot -- and edits pinned on screen forever
  // would be its own bug.
  // -------------------------------------------------------------------------

  await test('the backstop covers the batch and the queue ahead of it', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._pendingChanges.set('1,0', [360, 480]);
    h.card._saveChanges();

    // Per-write budget x batch size, plus the largest budget anything already
    // at the head of the device queue can hold. A queued operation carries no
    // watchdog until it reaches the head, so its own budget does not bound the
    // wait -- and the card's own untracked refresh_schedule (30 s) can be the
    // thing in front of it.
    assertEqual(soonestTimerDelay(), schedBackstop(2), 'batch size and queue slack both counted');
  });

  await test('progress extends the backstop to what is left', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._pendingChanges.set('1,0', [400, 500]);
    h.card._pendingChanges.set('2,0', [420, 540]);
    h.card._saveChanges();
    assertEqual(soonestTimerDelay(), schedBackstop(3), 'armed for three');

    h.settle(h.opIds()[0], 'accepted');
    assertEqual(soonestTimerDelay(), schedBackstop(2), 're-armed for the two still out');
  });

  await test('an unanswered write is released by the backstop', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();

    advance(schedBackstop(1) - 1);
    assertEqual(h.card._inFlight.size, 1, 'still waiting just before the deadline');

    advance(1);
    assertEqual(h.card._inFlight.size, 0, 'released at the deadline');
    assertEqual(h.card._writeErrors.length, 1, 'and reported rather than silently dropped');
    assertEqual(h.card._writeErrors[0].status, 'no confirmation', 'named for what happened');
    assertEqual(h.card._writeErrors[0].label, 'Mon layer 0', 'and attributed to its cell');
    assertEqual(h.card._pendingChanges.size, 1, 'the unconfirmed edit is kept');
    assertEqual(h.countOf('refresh_schedule'), 1, 'device state is re-read');
  });

  await test('the backstop reports every write still outstanding', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._pendingChanges.set('1,0', [400, 500]);
    h.card._saveChanges();

    advance(schedBackstop(2));
    assertEqual(h.card._writeErrors.length, 2, 'both are named, not just one');
  });

  await test('the backstop is disarmed once the batch settles', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();
    h.settle(h.opIdsFor('set_schedule_entry')[0], 'accepted');

    assertEqual(timerCount(), 0, 'no timer left armed');
    advance(600000);
    assertEqual(h.countOf('refresh_schedule'), 1, 'and no second refresh fires later');
  });

  await test('a re-attach mid-write does not strand the card', async () => {
    // Lovelace re-appends an existing card element when a masonry view
    // re-columns -- a resize, the sidebar toggling, entering edit mode. That
    // fires disconnectedCallback + connectedCallback on the same instance.
    // Teardown cancels the backstop, and _armBackstop is reachable only from
    // _saveChanges and _trackSingleEvent, both of which refuse to run while
    // _inFlight is populated -- so without a re-arm the card renders "saving…"
    // forever with Save and Discard disabled, recoverable only by reload.
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();

    h.card.disconnectedCallback();
    assertEqual(timerCount(), 0, 'teardown does cancel the timer');
    h.card.connectedCallback();
    assert(timerCount() > 0, 're-attach restores it');

    advance(schedBackstop(1));
    assertEqual(h.card._inFlight.size, 0, 'and it still releases the write');
    assertEqual(h.card._isSaving(), false, 'so the card is usable again');
    assertEqual(h.card._writeErrors[0].status, 'no confirmation', 'with the reason given');
  });

  await test('a re-attach resumes the deadline rather than restarting it', async () => {
    // A view that re-columns every few seconds would otherwise never reach a
    // deadline that keeps being reset to full.
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();
    const armed = soonestTimerDelay();

    h.card.disconnectedCallback();
    h.card.connectedCallback();
    assert(soonestTimerDelay() <= armed,
           'the resumed deadline is no later than the original');
  });

  await test('a re-attach with nothing in flight arms nothing', async () => {
    const h = makeCard();
    h.card.disconnectedCallback();
    h.card.connectedCallback();
    assertEqual(timerCount(), 0, 'no phantom backstop');
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
    assertEqual(h.card._writeErrors[0].label, 'Mon layer 0', 'and attributed');
    assertEqual(h.card._pendingChanges.size, 1, 'the edit is kept for a retry');
  });

  // -------------------------------------------------------------------------
  // Single events. Same shape, and worse numbers: a 3 s refresh against a 60 s
  // watchdog, plus an optimistic local delete that made a failed clear look
  // like a success for the better part of a minute.
  // -------------------------------------------------------------------------

  await test('a cleared event is held, not removed on faith', async () => {
    const h = makeCard({
      singleEvents: '[2] 2026-08-15 06:00 - 07:30 (run)\n[4] 2026-08-15 20:00 - 21:00 (run)',
    });
    h.card._clearSingleEvent(2);

    assertEqual(h.countOf('clear_single_event'), 1, 'the clear was issued');
    assertEqual(h.payloadsOf('clear_single_event')[0], '2', 'addressed to the slot asked for');
    assertEqual(h.card._singleEvents.length, 2, 'the row is still shown while unconfirmed');
    assert(h.card._slotInFlight(2), 'the cleared slot is marked in flight');
    assert(!h.card._slotInFlight(4), 'and the untouched one is not');

    h.settle(h.opIdsFor('clear_single_event')[0], 'accepted');
    assertEqual(h.countOf('refresh_single_events'), 1, 'the event list is re-read on settle');
    assertEqual(h.countOf('refresh_schedule'), 0, 'and the schedule is not, it did not change');
    assert(!h.card._slotInFlight(2), 'the in-flight mark is cleared');
  });

  await test('a failed clear reports instead of vanishing the row', async () => {
    const h = makeCard();
    h.card._clearSingleEvent(0);
    h.settle(h.opIdsFor('clear_single_event')[0], 'timeout', 'no response');

    assertEqual(h.card._singleEvents.length, 1, 'the event is still listed');
    assertEqual(h.card._writeErrors.length, 1, 'and the failure is visible');
    assertEqual(h.card._writeErrors[0].surface, 'single', 'attributed to the right surface');
  });

  await test('a single-event write waits on its own 60 s watchdog', async () => {
    const h = makeCard();
    h.card._scheduleQuickRun(30);
    assertEqual(h.countOf('set_single_event'), 1, 'the write was issued');
    assertEqual(soonestTimerDelay(), singleBackstop(1), 'backstop matches the firmware budget');
  });

  await test('a single-event write leaves no timer behind once it settles', async () => {
    const h = makeCard();
    h.card._scheduleQuickRun(30);
    h.settle(h.opIdsFor('set_single_event')[0], 'accepted');
    assertEqual(timerCount(), 0, 'no blind refresh timer survives');
  });

  await test('a Quick Run asks for the duration it was given', async () => {
    const h = makeCard();
    h.card._scheduleQuickRun(30);
    const [begin, end] = h.payloadsOf('set_single_event')[0].split(',').map(Number);
    assertEqual(end - begin, 30 * 60, 'the window is the requested duration');
    const now = Math.floor(Date.now() / 1000);
    assert(begin > now && begin <= now + 61, 'and it starts about a minute out');
  });

  await test('a second single-event write is refused while one is out', async () => {
    const h = makeCard();
    h.card._scheduleQuickRun(30);
    h.card._scheduleQuickRun(60);
    assertEqual(h.countOf('set_single_event'), 1, 'only the first was issued');
  });

  // -------------------------------------------------------------------------
  // Rendered state. The card refuses a second write while one is outstanding;
  // a button that still looks live and silently swallows the click is its own
  // bug, so the constraint has to reach the markup.
  // -------------------------------------------------------------------------

  await test('the toolbar reports saving and blocks both buttons', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._render();
    assertIncludes(h.html, 'data-action="save"', 'the Save button is wired');
    assertIncludes(h.html, 'unsaved', 'and the edit is flagged');

    h.card._saveChanges();
    assertIncludes(h.html, 'Saving…', 'the button says what is happening');
    assertIncludes(h.html, 'saving 1…', 'and the badge counts the writes out');
    assertIncludes(h.html, 'data-action="save" disabled', 'Save is disabled');
    assertIncludes(h.html, 'data-action="discard" disabled', 'and so is Discard');

    h.settle(h.opIdsFor('set_schedule_entry')[0], 'accepted');
    assertExcludes(h.html, 'Saving…', 'and the state clears when it settles');
  });

  await test('the Quick Run controls are disabled while a write is out', async () => {
    const h = makeCard();
    assertIncludes(h.html, 'data-action="quick-run-preset"', 'presets render');
    assertIncludes(h.html, 'data-action="clear-single-event" data-slot="0"',
                   'and the clear button addresses its slot');
    assertExcludes(h.html, 'data-minutes="30" disabled', 'live when idle');

    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();
    assertIncludes(h.html, 'data-minutes="30" disabled', 'presets go dead while saving');
    assertIncludes(h.html, 'data-slot="0"', 'the clear button is still addressed');
    assertIncludes(h.html, 'disabled', 'and disabled rather than silently inert');
  });

  await test('the row being removed says so', async () => {
    const h = makeCard();
    h.card._clearSingleEvent(0);
    assertIncludes(h.html, 'qr-event-row pending', 'the row is marked');
    assertIncludes(h.html, 'mdi:timer-sand', 'with a waiting icon');
    assertIncludes(h.html, 'Removing…', 'and a reason');
  });

  // -------------------------------------------------------------------------
  // Wire format. This is the bug class that shipped twice.
  // -------------------------------------------------------------------------

  await test('set_schedule_entry serialises layer, day, then times', async () => {
    // Day 3 / layer 2 on purpose: the fields are distinguishable only when
    // they differ, so a 0,0 fixture cannot catch a transposition.
    const h = makeCard();
    h.card._pendingChanges.set('3,2', [7 * 60 + 15, 9 * 60 + 45]);
    h.card._saveChanges();
    assertEqual(h.payloadsOf('set_schedule_entry')[0], '2,3,7,15,9,45', 'layer,day,sh,sm,eh,em');
  });

  await test('clear_schedule_entry serialises layer then day', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('5,1', null);
    h.card._saveChanges();
    assertEqual(h.payloadsOf('clear_schedule_entry')[0], '1,5', 'layer,day');
  });

  await test('a block at the right edge is clamped below hour 24', async () => {
    // api_bridge.cpp rejects hour > 23, so end === 1440 could never be saved.
    const h = makeCard();
    h.card._pendingChanges.set('3,2', [1380, 1440]);
    h.card._saveChanges();
    assertEqual(h.payloadsOf('set_schedule_entry')[0], '2,3,23,0,23,59',
                'serialised as 23:59, not 24:00');
  });

  await test('every service call carries a unique op_id', async () => {
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();
    h.settle(h.opIdsFor('set_schedule_entry')[0], 'accepted');
    h.card._scheduleQuickRun(30);

    for (const c of h.calls) {
      assert(c.domain === 'esphome', `${c.service} goes to the esphome domain`);
      assert(c.data && typeof c.data.op_id === 'string' && c.data.op_id.length > 0,
             `${c.service} carries an op_id`);
    }
    const ids = h.opIds();
    assertEqual(new Set(ids).size, ids.length, 'op_ids are unique');
  });

  // -------------------------------------------------------------------------
  // The event parser. Regression guard for a bug that rendered the whole Quick
  // Run list empty, indistinguishably from "no events exist".
  // -------------------------------------------------------------------------

  await test('the event parser matches the format the firmware emits', async () => {
    // format_single_events_display appends " (run)"/" (off)"; the card's regex
    // was anchored on the end time, so every line failed to match.
    const h = makeCard();
    h.card._parseSingleEvents(
      '[0] 2026-08-15 06:15 - 07:30 (run)\n[3] 2026-08-16 22:05 - 23:45 (off)');

    assertEqual(h.card._singleEvents.length, 2, 'both lines parsed');
    const [a, b] = h.card._singleEvents;
    assertEqual(a.slot, 0, 'slot read');
    assertEqual(a.action, 'run', 'action read');
    assertEqual(a.begin.getHours(), 6, 'begin hour');
    assertEqual(a.begin.getMinutes(), 15, 'begin minute');
    assertEqual(a.end.getHours(), 7, 'end hour');
    assertEqual(a.end.getMinutes(), 30, 'end minute');
    assertEqual(a.begin.getMonth(), 7, 'month is zero-based on the Date');
    assertEqual(a.begin.getDate(), 15, 'day of month');
    assertEqual(b.slot, 3, 'second slot read');
    assertEqual(b.action, 'off', 'vacation action read');
    assertEqual(b.end.getHours(), 23, 'second end hour');
  });

  await test('a line without the action suffix still parses, as run', async () => {
    const h = makeCard();
    h.card._parseSingleEvents('[1] 2026-08-15 06:00 - 07:30');
    assertEqual(h.card._singleEvents.length, 1, 'parsed');
    assertEqual(h.card._singleEvents[0].action, 'run', 'defaulted to run');
  });

  await test('the sentinel and unavailable states clear the list', async () => {
    const h = makeCard();
    for (const s of ['No single events', 'unavailable', 'unknown', '']) {
      h.card._parseSingleEvents('[0] 2026-08-15 06:00 - 07:30 (run)');
      assertEqual(h.card._singleEvents.length, 1, 'seeded');
      h.card._parseSingleEvents(s);
      // The list must be *reset*, not merely left unextended -- this runs on
      // every state change, so without the reset events accumulate forever.
      assertEqual(h.card._singleEvents.length, 0, `"${s}" clears the list`);
    }
  });

  await test('a malformed line is skipped without taking the others with it', async () => {
    const h = makeCard();
    h.card._parseSingleEvents(
      '[0] 2026-08-15 06:00 - 07:30 (run)\ngarbage\n[1] 2026-08-15 08:00 - 09:00 (off)');
    assertEqual(h.card._singleEvents.length, 2, 'the two good lines survive');
  });

  // -------------------------------------------------------------------------
  // Trust boundary and lifetime.
  // -------------------------------------------------------------------------

  await test('device strings cannot inject markup', async () => {
    // `detail` echoes the failed request ("parse error: <payload>"), so it is
    // influenced by anyone who can call the service, and it lands in innerHTML.
    // The payload repeats every metacharacter: an escaper that handles only the
    // first occurrence of each -- a non-global replace -- is a live XSS, and a
    // single-occurrence payload cannot tell the two apart.
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();
    h.settle(h.opIdsFor('set_schedule_entry')[0], 'invalid',
             '<b>x</b><img src=x onerror="alert(1)"><i a=\'b\'>&amp;');

    assertIncludes(
      h.html,
      '&lt;b&gt;x&lt;/b&gt;&lt;img src=x onerror=&quot;alert(1)&quot;&gt;'
        + '&lt;i a=&#39;b&#39;&gt;&amp;amp;',
      'every metacharacter is escaped, not just the first of each');
    assertExcludes(h.html, '<img', 'no tag is emitted raw');
    assertExcludes(h.html, 'onerror=&quot;alert(1)&quot;>', 'nor a handler that could close');
  });

  await test('a hostile status string is escaped too', async () => {
    // `status` comes off the same wire as `detail` and lands in the same
    // innerHTML; only `detail` being escaped would still be exploitable.
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();
    h.settle(h.opIdsFor('set_schedule_entry')[0], '<script>alert(1)</script><script>');

    assertExcludes(h.html, '<script>', 'the tag is not emitted raw');
    assertIncludes(h.html, '&lt;script&gt;alert(1)&lt;/script&gt;&lt;script&gt;',
                   'both occurrences escaped');
  });

  await test('the error label is escaped, whatever its source', async () => {
    // Labels are card-generated today ("Mon layer 0"), so this is not
    // currently reachable with hostile data -- which is exactly why it needs a
    // test. It asserts the *contract* of the renderer, so that a later change
    // making labels data-driven cannot quietly turn the panel into a sink.
    const h = makeCard();
    h.card._writeErrors = [
      { surface: 'schedule', label: '<img src=x onerror=alert(1)>', status: 'rejected', detail: '' },
    ];
    h.card._render();
    assertExcludes(h.html, '<img src=x', 'the label is not emitted raw');
    assertIncludes(h.html, '&lt;img src=x onerror=alert(1)&gt;', 'it is escaped');
  });

  await test('a settled surface stops being refreshed by the next one', async () => {
    // _refreshOnDrain is a set of surfaces to re-read when the batch drains.
    // If it is not cleared after firing, it leaks: a later single-event write
    // would drag a schedule re-read along with it, forever.
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();
    h.settle(h.opIdsFor('set_schedule_entry')[0], 'accepted');
    assertEqual(h.countOf('refresh_schedule'), 1, 'the schedule was re-read');

    h.card._clearSingleEvent(0);
    h.settle(h.opIdsFor('clear_single_event')[0], 'accepted');
    assertEqual(h.countOf('refresh_single_events'), 1, 'the event list was re-read');
    assertEqual(h.countOf('refresh_schedule'), 1,
                'and the schedule was not re-read a second time');
  });

  await test('a re-attach restores the subscription, not just the timer', async () => {
    // Teardown removes the settle subscription, and the only other place it is
    // restored is the `hass` setter. Without a re-subscribe here, a write that
    // settles between re-attach and Home Assistant's next state push lands on
    // a card that is not listening -- and the backstop then reports "no
    // confirmation" for a write the device accepted.
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();
    const opId = h.opIdsFor('set_schedule_entry')[0];

    h.card.disconnectedCallback();
    await flush();
    h.card.connectedCallback();
    assertEqual(h.subscribeCount, 2, 'the card re-subscribed');

    // Delivered through the new subscription, with no `hass` update in between.
    h.settle(opId, 'accepted');
    assertEqual(h.card._pendingChanges.size, 0, 'the settle was received');
    assertEqual(h.card._writeErrors.length, 0, 'and no false failure reported');
  });

  await test('a stale subscribe rejection does not clear a newer handle', async () => {
    // The rejection handler used to null _settleSub unconditionally. If a
    // detach/re-attach installed a newer subscription while the old promise
    // was still pending, the late rejection stranded it -- leaking that
    // subscription and letting the next _subscribeSettled stack a duplicate.
    const h = makeCard({ subscribeFails: true });
    await quietly(async () => {
      const stale = h.card._settleSub;
      h.card._settleSub = 'newer-subscription'; // as a re-attach would install
      await flush();
      assert(h.card._settleSub === 'newer-subscription',
             'the newer handle survives the older promise rejecting');
      assert(stale !== h.card._settleSub, 'and it is not the stale one');
      h.settleRejections();
    });
  });

  await test('op ids are unique across card instances', async () => {
    // Settle events are global and the sequence counter is per instance, so
    // two cards issuing their first call in the same millisecond would mint
    // the same id -- and each would consume the other's result.
    const a = makeCard();
    const b = makeCard();
    a.card._pendingChanges.set('0,0', [360, 480]);
    b.card._pendingChanges.set('0,0', [360, 480]);
    a.card._saveChanges();
    b.card._saveChanges();

    const idA = a.opIdsFor('set_schedule_entry')[0];
    const idB = b.opIdsFor('set_schedule_entry')[0];
    assert(idA !== idB, 'two cards do not mint the same op id');

    // And the consequence that matters: B's settle must not retire A's edit.
    a.settle(idB, 'accepted');
    assertEqual(a.card._pendingChanges.size, 1, "the other card's settle is not consumed");
  });

  await test('queue progress on our node extends the backstop', async () => {
    // WATCHDOG_QUEUE_HEAD_MS is the largest budget one operation ahead can
    // hold, not a bound on the queue: it has no depth limit and queued
    // operations carry no watchdog until they reach the head, so enough
    // foreign writes in front would outlast any fixed slack. Every completion
    // on our node fires a settle, so the deadline tracks the queue actually
    // draining instead.
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();

    advance(schedBackstop(1) - 1000); // nearly out of time
    h.foreignSettle('hwr-pump');      // something ahead of us finished
    assertEqual(soonestTimerDelay(), schedBackstop(1), 'the deadline was pushed back out');

    advance(schedBackstop(1) - 1);
    assertEqual(h.card._inFlight.size, 1, 'still waiting, not falsely failed');
  });

  await test('progress on a different node does not extend the backstop', async () => {
    // A second controller in the same install says nothing about our queue,
    // and letting it hold the deadline open would be a stuck card again.
    const h = makeCard();
    h.card._pendingChanges.set('0,0', [360, 480]);
    h.card._saveChanges();

    advance(schedBackstop(1) - 1000);
    h.foreignSettle('some-other-pump');
    assertEqual(soonestTimerDelay(), 1000, 'the deadline is unchanged');

    advance(1000);
    assertEqual(h.card._inFlight.size, 0, 'and it still fires on time');
  });

  await test('a foreign settle with nothing in flight changes nothing', async () => {
    const h = makeCard();
    h.foreignSettle('hwr-pump');
    assertEqual(timerCount(), 0, 'no backstop is armed out of nowhere');
    assertEqual(h.countOf('refresh_schedule'), 0, 'and nothing is refreshed');
  });

  await test('the card subscribes once and releases on teardown', async () => {
    const h = makeCard();
    assertEqual(h.subscribedType, 'esphome.alpha_hwr_write_settled', 'subscribed to the right event');

    const sub = h.card._settleSub;
    h.card.hass = h.hass; // a second hass update must not re-subscribe
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

    assertEqual(timerCount(), 0, 'no timer survives the card');
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
