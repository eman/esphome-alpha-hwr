/**
 * Alpha HWR Schedule Card — v6
 *
 * Custom Lovelace card for managing the Grundfos ALPHA HWR pump's weekly schedule.
 * Reads the schedule from the per-layer ESPHome read-back sensors and writes
 * changes via ESPHome services.
 *
 * Card config:
 *   type: custom:alpha-hwr-schedule-card
 *   device: hwr_pump                         # ESPHome device name — required.
 *                                            #   Used for service calls and to
 *                                            #   derive the default entity IDs.
 *   title: Pump Schedule                     # Optional
 *   layer_entities:                          # Optional — defaults to
 *     - sensor.hwr_pump_schedule_layer_0      #   sensor.<device>_schedule_layer_0..4
 *     - sensor.hwr_pump_schedule_layer_1
 *     - sensor.hwr_pump_schedule_layer_2
 *     - sensor.hwr_pump_schedule_layer_3
 *     - sensor.hwr_pump_schedule_layer_4
 *   enabled_entity: switch.hwr_pump_schedule_enabled          # Optional (default derived)
 *   single_events_entity: sensor.hwr_pump_single_events       # Optional (default derived)
 *   forecast_entity: sensor.dhw_..._forecast_weekly_series    # Optional (v6)
 *                                            #   Paints predicted demand as a
 *                                            #   heat strip behind each day.
 *   desired_entity: sensor.dhw_..._pump_schedule_series       # Optional (v6)
 *                                            #   Dashed ghosts for intervals
 *                                            #   the scheduler wants but the
 *                                            #   device is not holding.
 *
 * Schedule data model (current architecture):
 *   - Five per-layer text sensors `schedule_layer_0..4`, each a JSON array of 7
 *     day cells (Mon=0..Sun=6): [start_min,end_min] (enabled) or 0 (disabled).
 *       e.g. [[360,480],0,0,0,0,0,0]
 *   - Schedule on/off comes from the `Schedule Enabled` switch ("on"/"off").
 *   The card reassembles these into { e, s:{ layer: [7 cells] } } internally, so
 *   the rest of the card is unchanged.
 *
 * Single Events text sensor format:
 *   [slot] YYYY-MM-DD HH:MM - HH:MM
 *   (one line per active event)
 *
 * v6 Changes:
 *   - Optional forecast/desired overlays, both off unless configured, so
 *     existing card configs render exactly as before.
 *   - `forecast_entity` paints the weekly forecast's demand windows as a
 *     translucent heat strip behind each day row, so you can see whether a
 *     programmed burst actually lands in front of predicted demand — the one
 *     thing the grid alone could never show.
 *   - `desired_entity` outlines intervals the scheduler wants but the device
 *     is not holding, surfacing scheduler-vs-device drift at the place you
 *     would fix it. Matching intervals are not ghosted; they are already
 *     drawn as real blocks.
 *   - Both overlays are pointer-events:none and sit below the interactive
 *     blocks, so dragging and editing are untouched.
 *
 * v5 Changes:
 *   - The Enable/Disable Schedule button now toggles the `Schedule Enabled`
 *     switch entity (coupled) instead of calling the raw set_schedule_enabled
 *     service, so enabling the schedule from the card forces the pump to AUTO
 *     (never a dead STOP+schedule) and disabling stops it — matching the
 *     Engage Pump / Schedule Enabled mutual-exclusion model.
 *
 * v4 Changes:
 *   - Migrated off the removed aggregate "Weekly Schedule" JSON sensor to the
 *     per-layer read-back sensors + `Schedule Enabled` switch.
 *
 * v3 Changes:
 *   - Quick Run panel for one-time schedules (single events)
 *   - Green overlay bars on today's timeline for active events
 *   - Duration presets (30m, 1h, 2h, 4h) + custom datetime pickers
 *   - Active single events list with clear buttons
 */

const DAYS = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];
const DAYS_FULL = ['Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday', 'Sunday'];
const MINUTES_IN_DAY = 1440;
const HOUR_LABELS = [0, 3, 6, 9, 12, 15, 18, 21, 24];
const SNAP_MINUTES = 15;
const MIN_BLOCK_MINUTES = 15;
const MAX_LAYERS = 5; // pump supports layers 0-4
// Mirrors of the firmware's per-command watchdog budgets
// (write_operation_service.h). They bound how long a write can legitimately
// take, so they -- not a guessed interval -- are what the card waits before
// concluding a settle event is never coming. Writes are queued one at a time,
// so a batch's ceiling is per-write x batch size.
const WATCHDOG_SCHED_ENTRY_MS = 20000;
const WATCHDOG_SINGLE_EVENT_MS = 60000;
const SETTLE_MARGIN_MS = 5000; // event delivery through HA, on top of the budget
// Slack for whatever is ahead of us in the device's queue. The budgets above
// bound an operation's time *at the head*: arm_watchdog_ is called from
// start_front_, so a queued operation carries no timer at all until it gets
// there. The queue is shared with every other write source -- including the
// card's own untracked refreshes (30 s) and any entity write -- so the wait a
// tracked write actually faces is not bounded by its own budget. This is the
// largest budget anything ahead of it can hold (WATCHDOG_UPLOAD_MS).
//
// Erring long is deliberate: the backstop is the last resort, not the normal
// path, and a backstop that fires early is worse than one that fires late. It
// reports "no confirmation" for a write the device is still working on, drops
// the real settle when it arrives, and leaves the edit pending -- reporting
// failure for a write that succeeded.
const WATCHDOG_QUEUE_HEAD_MS = 150000;
const QUICK_RUN_PRESETS = [
  { label: '30m', minutes: 30 },
  { label: '1h', minutes: 60 },
  { label: '2h', minutes: 120 },
  { label: '4h', minutes: 240 },
];

class AlphaHwrScheduleCard extends HTMLElement {
  constructor() {
    super();
    this.attachShadow({ mode: 'open' });
    this._config = {};
    this._hass = null;
    this._schedule = null;
    this._selectedBlock = null; // { day, layer }
    this._dragging = null;
    this._hoverBlock = null; // { day, layer } for tooltip
    this._pendingChanges = new Map(); // "day,layer" -> [start,end] or null
    this._showApplyTo = false;
    this._applyDays = new Set();
    this._editingTime = false; // inline time editor open
    this._nowInterval = null;
    // Quick Run (single events) state
    this._showQuickRun = false;
    this._quickRunCustom = false;
    this._singleEvents = []; // parsed: [{slot, begin, end}]
    this._lastSingleEventsState = '';
    // Write tracking. Every service call the card makes carries an op_id, and
    // the device answers each one with exactly one esphome.alpha_hwr_write_settled
    // event (AGENTS §8.4 rule 4). _inFlight is op_id -> { key, entry, surface },
    // so a pending edit survives on screen until its own write is confirmed.
    this._inFlight = new Map();
    this._writeErrors = []; // [{ label, status, detail }] from the last save
    this._refreshOnDrain = new Set(); // 'schedule' | 'single'
    this._settleSub = null; // subscription promise, or null when unsubscribed
    this._settleBackstop = null;
    this._settleDeadline = 0; // absolute, so a re-attach resumes it
    this._backstopUnitMs = WATCHDOG_SCHED_ENTRY_MS; // per-write budget of the current batch
  }

  setConfig(config) {
    if (!config.device) throw new Error('Please define device (ESPHome device name)');
    const device = config.device;
    this._config = {
      device,
      title: config.title || 'Pump Schedule',
      // Per-layer schedule read-back sensors (schedule_layer_0..4). Override with
      // `layer_entities:` if your entity IDs differ from the derived defaults.
      layer_entities: config.layer_entities ||
        Array.from({ length: MAX_LAYERS }, (_, l) => `sensor.${device}_schedule_layer_${l}`),
      // Schedule on/off switch (state "on"/"off").
      enabled_entity: config.enabled_entity || `switch.${device}_schedule_enabled`,
      single_events_entity: config.single_events_entity || `sensor.${device}_single_events`,
      // Optional overlays, both unset by default so existing configs render
      // exactly as before.
      //   forecast_entity: a dhw-series-publisher weekly series sensor. Its
      //     `windows` attribute is painted as a heat strip behind each day,
      //     so you can see whether a burst actually lands in front of
      //     predicted demand.
      //   desired_entity: the pump schedule series. Intervals the scheduler
      //     wants but the device does not hold are outlined as ghosts,
      //     surfacing scheduler-vs-device drift where you would fix it.
      forecast_entity: config.forecast_entity || null,
      desired_entity: config.desired_entity || null,
    };
    this._forecastWindows = [];
    this._desiredGhosts = [];
    this._render();
  }

  connectedCallback() {
    if (this._config.device) this._render();
    // Lovelace re-appends an existing card element when a masonry view
    // re-columns -- a window resize, the sidebar toggling, entering edit mode
    // -- which fires disconnectedCallback and connectedCallback on the *same*
    // instance. Teardown cancels the backstop, so without this a write that is
    // in flight across a re-attach loses the only thing that would ever
    // release it: _armBackstop is reached only from _saveChanges and
    // _trackSingleEvent, and both refuse to run while _inFlight is non-empty.
    // The card would render "saving…" forever with Save and Discard disabled.
    this._rearmBackstop();
    // Update current-time line every minute
    this._nowInterval = setInterval(() => {
      const line = this.shadowRoot?.querySelector('.now-line');
      if (line) {
        const now = new Date();
        const mins = now.getHours() * 60 + now.getMinutes();
        line.style.left = `${(mins / MINUTES_IN_DAY) * 100}%`;
      }
    }, 60000);
  }

  disconnectedCallback() {
    if (this._nowInterval) clearInterval(this._nowInterval);
    this._nowInterval = null;
    if (this._settleBackstop) clearTimeout(this._settleBackstop);
    this._settleBackstop = null;
    // subscribeEvents resolves to an unsubscribe function. Teardown can beat
    // that resolution, so unsubscribe through the promise rather than storing
    // the function -- otherwise a card removed quickly leaks its subscription.
    const sub = this._settleSub;
    this._settleSub = null;
    if (sub) Promise.resolve(sub).then(unsub => { if (unsub) unsub(); }).catch(() => {});
  }

  set hass(hass) {
    this._hass = hass;
    this._subscribeSettled(hass);
    let needRender = false;

    // The schedule grid + enabled flag come from the per-layer read-back sensors
    // and the Schedule Enabled switch. Concatenate their states into a single
    // change signature so we only rebuild/re-render when something actually moves.
    const sig = this._config.layer_entities
      .map(e => (hass.states[e] ? hass.states[e].state : ''))
      .join('|') + '|' + (hass.states[this._config.enabled_entity]?.state ?? '')
      + '|' + (this._config.forecast_entity
        ? (hass.states[this._config.forecast_entity]?.last_changed ?? '') : '')
      + '|' + (this._config.desired_entity
        ? (hass.states[this._config.desired_entity]?.last_changed ?? '') : '');
    if (sig !== this._lastState) {
      this._parseOverlays(hass);
      this._lastState = sig;
      this._rebuildSchedule();
      needRender = true;
    }

    // Watch single events text sensor
    const seState = hass.states[this._config.single_events_entity];
    const newSE = seState ? seState.state : '';
    if (newSE !== this._lastSingleEventsState) {
      this._lastSingleEventsState = newSE;
      this._parseSingleEvents(newSE);
      needRender = true;
    }
    if (needRender) this._render();
  }

  /* ─── Write confirmation ─── */

  /** Subscribe to the device's terminal write events.
   *
   * The card used to fire its writes and immediately drop the edits that
   * produced them, then re-read the device on a 3 s timer. Neither number was
   * connected to anything: a `set_schedule_entry` carries a 20 s watchdog
   * (WATCHDOG_SCHED_ENTRY_MS) and writes are queued, so N edits can take N x
   * 20 s to settle. The 3 s read therefore returned the pre-write schedule,
   * which overwrote the user's edits with stale values -- and a write that
   * failed outright looked identical to one that succeeded, because the edit
   * had already been discarded either way.
   *
   * Every write answers with exactly one esphome.alpha_hwr_write_settled
   * event carrying the op_id the card generated, so correlate on that. No
   * node-name matching is needed (and none would be reliable -- the event's
   * `node` is App.get_name(), which HA slugifies independently of the card's
   * `device`); an op_id we did not issue is simply not in _inFlight.
   */
  _subscribeSettled(hass) {
    if (this._settleSub || !hass || !hass.connection ||
        typeof hass.connection.subscribeEvents !== 'function') return;
    try {
      this._settleSub = hass.connection.subscribeEvents(
        (ev) => this._onWriteSettled(ev), 'esphome.alpha_hwr_write_settled');
      Promise.resolve(this._settleSub).catch((err) => {
        console.error('[alpha-hwr-card] write_settled subscribe failed:', err);
        this._settleSub = null;
      });
    } catch (err) {
      console.error('[alpha-hwr-card] write_settled subscribe failed:', err);
      this._settleSub = null;
    }
  }

  _onWriteSettled(ev) {
    const data = (ev && ev.data) || {};
    const op = this._inFlight.get(data.op_id);
    if (!op) return; // someone else's write, or one we already resolved
    this._inFlight.delete(data.op_id);

    // CLAMPED means the device took the write and adjusted it -- the edit is
    // spent either way, and the refresh below shows what actually landed.
    if (data.status === 'accepted' || data.status === 'clamped') {
      this._resolvePending(op);
    } else {
      this._writeErrors.push({
        surface: op.surface,
        label: op.label,
        status: data.status || 'unknown',
        detail: data.detail || '',
      });
    }
    this._afterSettle();
  }

  /** Drop a confirmed edit -- but only if it is still the edit we wrote.
   *
   * Identity, not equality: every edit path stores a fresh array, so if the
   * user moved the same block again while the write was in flight, the map
   * now holds a different object and the newer intent must survive.
   */
  _resolvePending(op) {
    if (op.surface !== 'schedule') return;
    if (this._pendingChanges.get(op.key) === op.entry) this._pendingChanges.delete(op.key);
  }

  /** Called after every settle, and by the backstop timer. */
  _afterSettle() {
    if (this._inFlight.size > 0) {
      // Progress extends the deadline: what is left to wait for is what is
      // still outstanding, not what the batch started as.
      this._armBackstopForInFlight();
      this._render();
      return;
    }
    if (this._settleBackstop) clearTimeout(this._settleBackstop);
    this._settleBackstop = null;
    this._settleDeadline = 0;
    if (this._refreshOnDrain.has('schedule')) this._callRefresh();
    if (this._refreshOnDrain.has('single')) this._callRefreshSingleEvents();
    this._refreshOnDrain.clear();
    this._render();
  }

  /** Give up waiting.
   *
   * A settle event can genuinely never arrive -- HA restarts, the websocket
   * drops and reconnects, the node reboots mid-write. Rather than pin the
   * user's edits on screen forever, release them after the device's own
   * watchdog has had time to fire, and say so.
   */
  _armBackstop(ms) {
    if (this._settleBackstop) clearTimeout(this._settleBackstop);
    // Absolute, so a re-attach resumes the original deadline rather than
    // restarting it -- a card that re-columns every few seconds would
    // otherwise never reach one.
    this._settleDeadline = Date.now() + ms;
    this._settleBackstop = setTimeout(() => {
      this._settleBackstop = null;
      this._settleDeadline = 0;
      for (const op of this._inFlight.values()) {
        this._writeErrors.push({
          surface: op.surface, label: op.label, status: 'no confirmation', detail: '',
        });
      }
      this._inFlight.clear();
      this._afterSettle();
    }, ms);
  }

  /** Arm the backstop for the work still outstanding, plus queue slack. */
  _armBackstopForInFlight() {
    this._armBackstop(this._inFlight.size * this._backstopUnitMs +
                      WATCHDOG_QUEUE_HEAD_MS + SETTLE_MARGIN_MS);
  }

  /** Restore the backstop after a re-attach, honouring the original deadline. */
  _rearmBackstop() {
    if (this._settleBackstop || this._inFlight.size === 0) return;
    if (!this._settleDeadline) return;
    this._armBackstop(Math.max(0, this._settleDeadline - Date.now()));
  }

  /** Rebuild the internal schedule model { e, s:{ layer: [7 cells] } } from the
   *  per-layer read-back sensors and the Schedule Enabled switch. Each layer
   *  sensor is a JSON array of 7 cells: [start_min,end_min] or 0. Malformed or
   *  not-yet-cached layers are skipped so the rest of the grid still renders. */
  _rebuildSchedule() {
    if (!this._hass) { this._schedule = null; return; }
    const s = {};
    let anyLayer = false;
    this._config.layer_entities.forEach((ent, l) => {
      const st = this._hass.states[ent];
      if (!st) return;
      const raw = st.state;
      if (!raw || !raw.startsWith('[')) return; // '', 'unknown', 'unavailable'
      try {
        const arr = JSON.parse(raw);
        if (Array.isArray(arr) && arr.length === 7) {
          s[String(l)] = arr;
          anyLayer = true;
        }
      } catch (e) { /* skip a malformed layer, keep the others */ }
    });

    const enSt = this._hass.states[this._config.enabled_entity];
    const enabled = enSt
      ? (enSt.state === 'on' || enSt.state === '1' || enSt.state === 'true')
      : false;

    // Keep null (→ "empty" rendering) until at least one source reports, so the
    // card doesn't flash an empty grid before the pump state is cached.
    this._schedule = (anyLayer || enSt) ? { e: enabled ? 1 : 0, s } : null;
  }

  /** Parse single events text sensor: "[slot] YYYY-MM-DD HH:MM - HH:MM" per line */
  _parseSingleEvents(stateStr) {
    this._singleEvents = [];
    if (!stateStr || stateStr === 'No single events' || stateStr === 'unavailable' || stateStr === 'unknown') return;
    const lines = stateStr.split('\n');
    // The firmware appends an action suffix -- " (run)" / " (off)" -- to each
    // line (format_single_events_display). The old pattern anchored on the end
    // time with `$`, so every line stopped matching and the Quick Run list and
    // today's overlay bars silently rendered empty. Accept an optional suffix.
    const re = /^\[(\d+)\]\s+(\d{4})-(\d{2})-(\d{2})\s+(\d{2}):(\d{2})\s*-\s*(\d{2}):(\d{2})(?:\s*\(([a-z]+)\))?$/;
    for (const line of lines) {
      const m = line.trim().match(re);
      if (m) {
        const slot = parseInt(m[1]);
        const begin = new Date(parseInt(m[2]), parseInt(m[3]) - 1, parseInt(m[4]), parseInt(m[5]), parseInt(m[6]));
        const end = new Date(parseInt(m[2]), parseInt(m[3]) - 1, parseInt(m[4]), parseInt(m[7]), parseInt(m[8]));
        const action = m[9] || 'run';
        this._singleEvents.push({ slot, begin, end, action });
      }
    }
  }

  /** Get single events that overlap with today, as minute ranges */
  _getTodaySingleEvents() {
    const now = new Date();
    const todayStart = new Date(now.getFullYear(), now.getMonth(), now.getDate());
    const todayEnd = new Date(todayStart.getTime() + 86400000);
    const events = [];
    for (const ev of this._singleEvents) {
      if (ev.end > todayStart && ev.begin < todayEnd) {
        const startMins = Math.max(0, (ev.begin - todayStart) / 60000);
        const endMins = Math.min(MINUTES_IN_DAY, (ev.end - todayStart) / 60000);
        events.push({ slot: ev.slot, start: Math.round(startMins), end: Math.round(endMins) });
      }
    }
    return events;
  }

  /* ─── Data helpers ─── */

  /** Get all blocks for a given day across all layers, merged with pending changes.
   *  Returns array of { layer, start, end, pending } */
  _getDayBlocks(day) {
    const blocks = [];
    const layers = (this._schedule && this._schedule.s) || {};

    for (let l = 0; l < MAX_LAYERS; l++) {
      const key = `${day},${l}`;
      let entry;
      if (this._pendingChanges.has(key)) {
        entry = this._pendingChanges.get(key); // null = deleted, [s,e] = set
      } else {
        const layerData = layers[String(l)];
        entry = layerData ? (layerData[day] || 0) : 0;
      }
      if (entry && Array.isArray(entry)) {
        blocks.push({
          layer: l,
          start: entry[0],
          end: entry[1],
          pending: this._pendingChanges.has(key),
        });
      }
    }
    return blocks;
  }

  /** Find the first available layer slot for a day */
  _findFreeLayer(day) {
    const used = new Set(this._getDayBlocks(day).map(b => b.layer));
    for (let l = 0; l < MAX_LAYERS; l++) {
      if (!used.has(l)) return l;
    }
    return -1;
  }

  _minutesToTime(mins) {
    const h = Math.floor(mins / 60);
    const m = mins % 60;
    return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}`;
  }

  _snapMinutes(mins) {
    return Math.max(0, Math.min(MINUTES_IN_DAY, Math.round(mins / SNAP_MINUTES) * SNAP_MINUTES));
  }

  _hasPendingChanges() {
    return this._pendingChanges.size > 0;
  }

  /** True while at least one write is out and unconfirmed. */
  _isSaving() {
    return this._inFlight.size > 0;
  }

  /** True if a clear_single_event for this slot is out and unconfirmed. */
  _slotInFlight(slot) {
    for (const op of this._inFlight.values()) {
      if (op.clearedSlot === slot) return true;
    }
    return false;
  }

  /* ─── Render ─── */

  _render() {
    const enabled = this._schedule ? this._schedule.e : 0;
    const hasPending = this._hasPendingChanges();
    const saving = this._isSaving();
    const now = new Date();
    const nowMins = now.getHours() * 60 + now.getMinutes();
    const nowPct = (nowMins / MINUTES_IN_DAY) * 100;

    // Count scheduled days
    let scheduledDays = 0;
    for (let d = 0; d < 7; d++) {
      if (this._getDayBlocks(d).length > 0) scheduledDays++;
    }

    this.shadowRoot.innerHTML = `
      <style>${this._getStyles(enabled)}</style>
      <ha-card>
        <div class="header">
          <div class="title-row">
            <ha-icon icon="mdi:pump" style="--mdc-icon-size:20px;color:var(--primary-color);margin-right:8px;"></ha-icon>
            <span class="title">${this._config.title}</span>
          </div>
          <div class="header-actions">
            <span class="schedule-summary">${scheduledDays} of 7 days</span>
            <button class="quick-run-chip ${this._showQuickRun ? 'active' : ''}" data-action="toggle-quick-run">
              <ha-icon icon="mdi:flash" style="--mdc-icon-size:14px;margin-right:3px"></ha-icon>Quick Run
            </button>
            ${saving ? `<span class="unsaved-badge saving">saving ${this._inFlight.size}…</span>`
    : hasPending ? '<span class="unsaved-badge">unsaved</span>' : ''}
          </div>
        </div>

        <div class="grid-container">
          <div class="hour-labels">
            ${HOUR_LABELS.map(h => {
      const pct = (h / 24) * 100;
      const label = h === 24 ? '' : (h === 0 ? '12a' : h < 12 ? `${h}a` : h === 12 ? '12p' : `${h - 12}p`);
      return `<span class="hour-label" style="left:${pct}%">${label}</span>`;
    }).join('')}
          </div>

          ${DAYS.map((day, i) => this._renderDayRow(day, i, nowPct)).join('')}
        </div>

        ${this._renderSelectionPanel()}
        ${this._renderQuickRunPanel()}

        <div class="footer">
          <div class="status">
            <span class="status-dot ${enabled ? 'active' : ''}"></span>
            <button class="btn ${enabled ? 'btn-outline' : 'btn-primary'}" data-action="toggle-schedule">
              ${enabled ? 'Disable' : 'Enable'} Schedule
            </button>
          </div>
          <div class="actions">
            <button class="btn btn-icon" data-action="refresh" title="Refresh from pump">
              <ha-icon icon="mdi:refresh" style="--mdc-icon-size:18px"></ha-icon>
            </button>
            ${hasPending ? `
              <button class="btn btn-outline" data-action="discard" ${saving ? 'disabled' : ''}>Discard</button>
              <button class="btn btn-primary btn-save" data-action="save" ${saving ? 'disabled' : ''}>
                <ha-icon icon="mdi:content-save" style="--mdc-icon-size:16px;margin-right:4px"></ha-icon>
                ${saving ? 'Saving…' : 'Save'}
              </button>
            ` : ''}
          </div>
        </div>
        ${this._renderWriteErrors()}
      </ha-card>
    `;

    this._attachEvents();
  }

  /** Escape for interpolation into the innerHTML template.
   *
   * `detail` on a settle event is built by the device and echoes the request
   * that failed ("parse error: <payload>"), so it is attacker-influenced by
   * anyone who can call the service. Everything from the wire gets escaped
   * before it reaches innerHTML.
   */
  _esc(s) {
    return String(s === undefined || s === null ? '' : s)
      .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
  }

  /** Report writes the device did not accept.
   *
   * The point of the whole settle path: a failed write used to be
   * indistinguishable from a successful one, because the edit was discarded
   * before either answer arrived.
   */
  _renderWriteErrors() {
    if (this._writeErrors.length === 0) return '';
    return `
      <div class="write-errors">
        <ha-icon icon="mdi:alert-circle-outline" style="--mdc-icon-size:16px"></ha-icon>
        <div class="write-errors-list">
          ${this._writeErrors.map(e => `<div class="write-error">
            <strong>${this._esc(e.label)}</strong>: ${this._esc(e.status)}${e.detail ? ` — ${this._esc(e.detail)}` : ''}
          </div>`).join('')}
          <div class="write-error-hint">Edits are still pending; press Save to retry.</div>
        </div>
      </div>`;
  }

  /** Fold dated intervals onto the card's (weekday, minute-of-day) grid.
   *
   * The grid is weekly-recurring while the series carry concrete dates, so
   * an interval is projected onto the weekday it falls on. An interval that
   * spans midnight is split, otherwise it would wrap and paint backwards
   * across the row.
   */
  _foldToGrid(intervals, valueOf) {
    const out = [];
    (intervals || []).forEach(iv => {
      let start = new Date(iv.s !== undefined ? iv.s : iv[0]);
      const end = new Date(iv.e !== undefined ? iv.e : iv[1]);
      if (!(start < end)) return;
      let guard = 0;
      while (start < end && guard++ < 10) {
        const day = (start.getDay() + 6) % 7; // JS 0=Sun -> card 0=Mon
        const startMin = start.getHours() * 60 + start.getMinutes();
        const midnight = new Date(start);
        midnight.setHours(24, 0, 0, 0);
        const segEnd = end < midnight ? end : midnight;
        const endMin = segEnd.getHours() === 0 && segEnd.getMinutes() === 0
          ? MINUTES_IN_DAY
          : segEnd.getHours() * 60 + segEnd.getMinutes();
        out.push({ day, start: startMin, end: endMin,
                   value: valueOf ? valueOf(iv) : 1 });
        start = midnight;
      }
    });
    return out;
  }

  _parseOverlays(hass) {
    this._forecastWindows = [];
    this._desiredGhosts = [];

    if (this._config.forecast_entity) {
      const st = hass.states[this._config.forecast_entity];
      // windows are [start_iso, end_iso, peak_pct] triples.
      const wins = st?.attributes?.windows;
      if (Array.isArray(wins)) {
        this._forecastWindows = this._foldToGrid(
          wins.map(w => ({ s: w[0], e: w[1], peak: w[2] })),
          iv => iv.peak,
        );
      }
    }

    if (this._config.desired_entity) {
      const st = hass.states[this._config.desired_entity];
      const desired = st?.attributes?.desired;
      const programmed = st?.attributes?.programmed;
      if (Array.isArray(desired)) {
        // Only show what the device is NOT holding — matching intervals are
        // already drawn as real blocks, so ghosting them all would be noise.
        const held = new Set((programmed || []).map(iv => `${iv.s}|${iv.e}`));
        this._desiredGhosts = this._foldToGrid(
          desired.filter(iv => !held.has(`${iv.s}|${iv.e}`)));
      }
    }
  }

  _renderDayRow(day, dayIdx, nowPct) {
    const blocks = this._getDayBlocks(dayIdx);
    const isToday = new Date().getDay() === (dayIdx + 1) % 7; // JS: 0=Sun, card: 0=Mon

    const forecastHtml = this._forecastWindows
      .filter(w => w.day === dayIdx)
      .map(w => {
        const leftPct = (w.start / MINUTES_IN_DAY) * 100;
        const widthPct = ((w.end - w.start) / MINUTES_IN_DAY) * 100;
        const opacity = Math.min(0.10 + (w.value || 0) / 100 * 0.30, 0.42);
        return `<div class="forecast-band"
             style="left:${leftPct}%;width:${Math.max(widthPct, 0.4)}%;
                    opacity:${opacity.toFixed(2)}"
             title="predicted demand ${w.value}%"></div>`;
      }).join('');

    const ghostHtml = this._desiredGhosts
      .filter(g => g.day === dayIdx)
      .map(g => {
        const leftPct = (g.start / MINUTES_IN_DAY) * 100;
        const widthPct = ((g.end - g.start) / MINUTES_IN_DAY) * 100;
        return `<div class="desired-ghost"
             style="left:${leftPct}%;width:${Math.max(widthPct, 0.7)}%"
             title="scheduler wants this; device is not holding it"></div>`;
      }).join('');

    const blocksHtml = blocks.map(b => {
      const leftPct = (b.start / MINUTES_IN_DAY) * 100;
      const widthPct = ((b.end - b.start) / MINUTES_IN_DAY) * 100;
      const sel = this._selectedBlock && this._selectedBlock.day === dayIdx && this._selectedBlock.layer === b.layer;
      const hov = this._hoverBlock && this._hoverBlock.day === dayIdx && this._hoverBlock.layer === b.layer;

      return `
        <div class="time-block ${sel ? 'selected' : ''} ${b.pending ? 'pending' : ''}"
             style="left:${leftPct}%;width:${Math.max(widthPct, 0.7)}%"
             data-day="${dayIdx}" data-layer="${b.layer}">
          <div class="drag-handle start" data-day="${dayIdx}" data-layer="${b.layer}" data-edge="start"></div>
          <div class="drag-handle end" data-day="${dayIdx}" data-layer="${b.layer}" data-edge="end"></div>
          ${(hov || sel) ? `<div class="block-tooltip">${this._minutesToTime(b.start)} – ${this._minutesToTime(b.end)}</div>` : ''}
        </div>`;
    }).join('');

    // Single event overlays (green bars on today)
    let singleEventHtml = '';
    if (isToday) {
      const todayEvents = this._getTodaySingleEvents();
      singleEventHtml = todayEvents.map(ev => {
        const leftPct = (ev.start / MINUTES_IN_DAY) * 100;
        const widthPct = ((ev.end - ev.start) / MINUTES_IN_DAY) * 100;
        return `<div class="single-event-bar" style="left:${leftPct}%;width:${Math.max(widthPct, 0.5)}%" title="One-time event (slot ${ev.slot})"></div>`;
      }).join('');
    }

    return `
      <div class="day-row ${isToday ? 'today' : ''}">
        <div class="day-label ${isToday ? 'today-label' : ''}">${day}</div>
        <div class="timeline-bar" data-day="${dayIdx}">
          ${forecastHtml}
          ${ghostHtml}
          ${isToday ? `<div class="now-line" style="left:${nowPct}%"></div>` : ''}
          ${singleEventHtml}
          ${blocksHtml}
        </div>
      </div>`;
  }

  _renderSelectionPanel() {
    if (!this._selectedBlock) return '';
    const { day, layer } = this._selectedBlock;
    const blocks = this._getDayBlocks(day);
    const block = blocks.find(b => b.layer === layer);

    if (!block) {
      // Selected day but the block was deleted — show add button
      return `
        <div class="selection-panel">
          <div class="sel-header">
            <span class="sel-day">${DAYS_FULL[day]}</span>
            <span class="sel-hint">No schedule</span>
            <button class="btn btn-primary btn-sm" data-action="add" data-day="${day}" style="margin-left:auto">
              <ha-icon icon="mdi:plus" style="--mdc-icon-size:16px;margin-right:2px"></ha-icon> Add
            </button>
          </div>
        </div>`;
    }

    const timeDisplay = this._editingTime ? this._renderTimeEditor(block) : `
      <button class="time-display" data-action="edit-time" title="Click to edit times">
        ${this._minutesToTime(block.start)} – ${this._minutesToTime(block.end)}
        <ha-icon icon="mdi:pencil" style="--mdc-icon-size:14px;margin-left:4px;opacity:0.5"></ha-icon>
      </button>`;

    const applyToHtml = this._showApplyTo ? `
      <div class="apply-to-section">
        <div class="apply-to-label">Apply this schedule to:</div>
        <div class="apply-to-days">
          ${DAYS.map((d, i) => {
      if (i === day) return '';
      const checked = this._applyDays.has(i);
      return `<label class="day-check ${checked ? 'checked' : ''}">
              <input type="checkbox" data-apply-day="${i}" ${checked ? 'checked' : ''}/>${d}
            </label>`;
    }).join('')}
        </div>
        <div class="apply-to-actions">
          <button class="btn btn-sm btn-outline" data-action="select-weekdays">Weekdays</button>
          <button class="btn btn-sm btn-outline" data-action="select-all-days">All</button>
          <button class="btn btn-sm btn-primary" data-action="confirm-apply" ${this._applyDays.size === 0 ? 'disabled' : ''}>
            Apply to ${this._applyDays.size} day${this._applyDays.size !== 1 ? 's' : ''}
          </button>
        </div>
      </div>` : '';

    return `
      <div class="selection-panel">
        <div class="sel-header">
          <span class="sel-day">${DAYS_FULL[day]}</span>
          ${timeDisplay}
          <div class="sel-actions">
            <button class="btn btn-sm btn-outline" data-action="apply-to" title="Copy to other days">
              <ha-icon icon="mdi:content-copy" style="--mdc-icon-size:16px;margin-right:2px"></ha-icon> Apply to…
            </button>
            <button class="btn btn-sm btn-danger" data-action="delete" data-day="${day}" data-layer="${layer}" title="Remove this block">
              <ha-icon icon="mdi:delete-outline" style="--mdc-icon-size:16px"></ha-icon>
            </button>
          </div>
        </div>
        ${applyToHtml}
      </div>`;
  }

  _renderTimeEditor(block) {
    const startH = Math.floor(block.start / 60);
    const startM = block.start % 60;
    const endH = Math.floor(block.end / 60);
    const endM = block.end % 60;

    const hourOpts = (sel) => Array.from({ length: 25 }, (_, i) =>
      `<option value="${i}" ${i === sel ? 'selected' : ''}>${String(i).padStart(2, '0')}</option>`
    ).join('');
    const minOpts = (sel) => [0, 15, 30, 45].map(m =>
      `<option value="${m}" ${m === sel ? 'selected' : ''}>${String(m).padStart(2, '0')}</option>`
    ).join('');

    return `
      <div class="time-editor">
        <select class="time-sel" data-field="startH">${hourOpts(startH)}</select>
        <span class="time-colon">:</span>
        <select class="time-sel" data-field="startM">${minOpts(startM)}</select>
        <span class="time-dash">–</span>
        <select class="time-sel" data-field="endH">${hourOpts(endH)}</select>
        <span class="time-colon">:</span>
        <select class="time-sel" data-field="endM">${minOpts(endM)}</select>
        <button class="btn btn-sm btn-primary" data-action="save-time">OK</button>
      </div>`;
  }

  _renderQuickRunPanel() {
    if (!this._showQuickRun) return '';
    const now = new Date();
    // Only one write may be outstanding at a time. Render that, rather than
    // letting a live-looking button swallow the click.
    const busy = this._isSaving();

    // Custom time picker defaults
    const defStartH = now.getHours();
    const defStartM = Math.ceil(now.getMinutes() / 15) * 15;
    const defEndH = Math.min(23, defStartH + 1);
    const defEndM = defStartM;

    const hourOpts = (sel) => Array.from({ length: 24 }, (_, i) =>
      `<option value="${i}" ${i === sel ? 'selected' : ''}>${String(i).padStart(2, '0')}</option>`
    ).join('');
    const minOpts = (sel) => [0, 15, 30, 45].map(m =>
      `<option value="${m}" ${m === (sel % 60) ? 'selected' : ''}>${String(m).padStart(2, '0')}</option>`
    ).join('');

    const dateStr = (d) => `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;

    // Active single events list
    const eventsHtml = this._singleEvents.length > 0 ? `
      <div class="qr-events">
        <div class="qr-events-label">Active one-time events:</div>
        ${this._singleEvents.map(ev => {
      const bStr = `${String(ev.begin.getHours()).padStart(2, '0')}:${String(ev.begin.getMinutes()).padStart(2, '0')}`;
      const eStr = `${String(ev.end.getHours()).padStart(2, '0')}:${String(ev.end.getMinutes()).padStart(2, '0')}`;
      const dStr = `${String(ev.begin.getMonth() + 1).padStart(2, '0')}/${String(ev.begin.getDate()).padStart(2, '0')}`;
      const removing = this._slotInFlight(ev.slot);
      // Disabled for *any* outstanding write, not just this row's: a second
      // clear would be refused, and a button that looks live but does nothing
      // is worse than one that says why.
      return `<div class="qr-event-row ${removing ? 'pending' : ''}">
            <span class="qr-event-time">${dStr} ${bStr}–${eStr}</span>
            <button class="qr-event-clear" data-action="clear-single-event" data-slot="${ev.slot}"
                    title="${removing ? 'Removing…' : busy ? 'Waiting for the current write' : 'Remove'}"
                    ${busy ? 'disabled' : ''}>
              <ha-icon icon="${removing ? 'mdi:timer-sand' : 'mdi:close'}" style="--mdc-icon-size:14px"></ha-icon>
            </button>
          </div>`;
    }).join('')}
      </div>` : '';

    const customPicker = this._quickRunCustom ? `
      <div class="qr-custom-section">
        <div class="qr-custom-row">
          <span class="qr-custom-label">Start</span>
          <input type="date" class="qr-date-input" data-field="qr-start-date" value="${dateStr(now)}">
          <select class="time-sel" data-field="qrStartH">${hourOpts(defStartH)}</select>
          <span class="time-colon">:</span>
          <select class="time-sel" data-field="qrStartM">${minOpts(defStartM)}</select>
        </div>
        <div class="qr-custom-row">
          <span class="qr-custom-label">End</span>
          <input type="date" class="qr-date-input" data-field="qr-end-date" value="${dateStr(now)}">
          <select class="time-sel" data-field="qrEndH">${hourOpts(defEndH)}</select>
          <span class="time-colon">:</span>
          <select class="time-sel" data-field="qrEndM">${minOpts(defEndM)}</select>
        </div>
        <button class="btn btn-sm btn-primary" data-action="schedule-custom-run" style="margin-top:6px"
                ${busy ? 'disabled' : ''}>
          <ha-icon icon="mdi:calendar-plus" style="--mdc-icon-size:15px;margin-right:4px"></ha-icon> Schedule
        </button>
      </div>` : '';

    return `
      <div class="quick-run-panel">
        <div class="qr-header">
          <ha-icon icon="mdi:flash" style="--mdc-icon-size:18px;color:var(--qr-color)"></ha-icon>
          <span class="qr-title">Quick Run</span>
          <span class="qr-subtitle">One-time pump activation</span>
        </div>
        <div class="qr-presets">
          ${QUICK_RUN_PRESETS.map(p => `
            <button class="qr-preset" data-action="quick-run-preset" data-minutes="${p.minutes}" ${busy ? 'disabled' : ''}>${p.label}</button>
          `).join('')}
          <button class="qr-preset qr-preset-custom ${this._quickRunCustom ? 'active' : ''}" data-action="toggle-quick-run-custom">
            <ha-icon icon="mdi:clock-edit-outline" style="--mdc-icon-size:15px;margin-right:3px"></ha-icon>Custom
          </button>
        </div>
        ${customPicker}
        ${eventsHtml}
      </div>`;
  }

  /* ─── Styles ─── */

  _getStyles(enabled) {
    return `
      :host {
        display: block;
        --primary-color: var(--ha-card-header-color, #4fc3f7);
        --block-gradient-start: #29b6f6;
        --block-gradient-end: #0288d1;
        --block-hover: #039be5;
        --block-selected: #ff9800;
        --bg-bar: var(--secondary-background-color, #f5f5f5);
        --text-color: var(--primary-text-color, #212121);
        --text-secondary: var(--secondary-text-color, #757575);
        --card-bg: var(--ha-card-background, var(--card-background-color, white));
        --pending-stripe: #ffb74d;
        --danger: #ef5350;
        --radius: 8px;
        --qr-color: #66bb6a;
      }
      ha-card {
        padding: 16px;
        overflow: visible;
      }

      /* ─ Header ─ */
      .header {
        display: flex;
        justify-content: space-between;
        align-items: center;
        margin-bottom: 14px;
      }
      .title-row {
        display: flex;
        align-items: center;
      }
      .title {
        font-size: 1.1em;
        font-weight: 600;
        color: var(--text-color);
        letter-spacing: 0.01em;
      }
      .header-actions {
        display: flex;
        align-items: center;
        gap: 8px;
      }
      .schedule-summary {
        font-size: 0.8em;
        color: var(--text-secondary);
        background: var(--bg-bar);
        padding: 3px 10px;
        border-radius: 12px;
      }
      .unsaved-badge {
        font-size: 0.72em;
        font-weight: 600;
        color: white;
        background: var(--block-selected);
        padding: 2px 8px;
        border-radius: 10px;
        animation: pulse-badge 2s infinite;
      }
      .unsaved-badge.saving {
        background: var(--secondary-text-color, #888);
      }
      @keyframes pulse-badge {
        0%, 100% { opacity: 1; }
        50% { opacity: 0.6; }
      }
      .btn[disabled] {
        opacity: 0.55;
        cursor: default;
        pointer-events: none;
      }
      .write-errors {
        display: flex;
        gap: 8px;
        align-items: flex-start;
        margin: 0 12px 12px;
        padding: 8px 10px;
        border-radius: 8px;
        color: var(--danger);
        background: color-mix(in srgb, var(--danger) 10%, transparent);
        font-size: 0.82em;
      }
      .write-error strong { font-weight: 600; }
      .write-error-hint {
        margin-top: 4px;
        color: var(--secondary-text-color, #888);
      }
      .qr-event-row.pending {
        opacity: 0.5;
      }

      /* ─ Grid ─ */
      .grid-container {
        position: relative;
      }
      .hour-labels {
        display: flex;
        margin-left: 44px;
        margin-bottom: 2px;
        position: relative;
        height: 18px;
      }
      .hour-label {
        position: absolute;
        font-size: 0.68em;
        font-weight: 500;
        color: var(--text-secondary);
        transform: translateX(-50%);
        letter-spacing: 0.02em;
      }

      /* ─ Day Row ─ */
      .day-row {
        display: flex;
        align-items: center;
        margin-bottom: 3px;
        height: 34px;
        transition: background 0.15s;
        border-radius: 4px;
        padding: 0 2px;
      }
      .day-row:hover {
        background: color-mix(in srgb, var(--bg-bar) 50%, transparent);
      }
      .day-row.today {
        background: color-mix(in srgb, var(--primary-color) 6%, transparent);
      }
      .day-label {
        width: 36px;
        font-size: 0.82em;
        font-weight: 500;
        color: var(--text-secondary);
        flex-shrink: 0;
        text-align: right;
        padding-right: 8px;
        user-select: none;
      }
      .today-label {
        color: var(--primary-color);
        font-weight: 700;
      }
      .timeline-bar {
        flex: 1;
        height: 28px;
        background: var(--bg-bar);
        border-radius: 6px;
        position: relative;
        cursor: pointer;
        overflow: visible;
        touch-action: none;
        transition: box-shadow 0.15s;
      }
      .timeline-bar:hover {
        box-shadow: inset 0 0 0 1px color-mix(in srgb, var(--primary-color) 30%, transparent);
      }

      /* ─ Now Line ─ */
      .now-line {
        position: absolute;
        top: -2px;
        bottom: -2px;
        width: 2px;
        background: var(--danger);
        border-radius: 1px;
        z-index: 15;
        pointer-events: none;
      }
      .now-line::before {
        content: '';
        position: absolute;
        top: -3px;
        left: -3px;
        width: 8px;
        height: 8px;
        border-radius: 50%;
        background: var(--danger);
      }

      /* ─ Single Event Overlay ─ */
      .single-event-bar {
        position: absolute;
        top: 1px;
        bottom: 1px;
        background: linear-gradient(135deg, rgba(76,175,80,0.55), rgba(56,142,60,0.55));
        border-radius: 4px;
        pointer-events: none;
        z-index: 4;
        border: 1px solid rgba(76,175,80,0.4);
      }

      /* ─ Time Block ─ */
      /* Optional overlays (forecast_entity / desired_entity). pointer-events
         are disabled so they never intercept a drag on a real block. */
      .forecast-band {
        position: absolute;
        top: 0; bottom: 0;
        background: var(--info-color, #2a78d6);
        pointer-events: none;
        z-index: 0;
        border-radius: 2px;
      }
      .desired-ghost {
        position: absolute;
        top: 3px; bottom: 3px;
        border: 1px dashed var(--warning-color, #eebf41);
        border-radius: 3px;
        background: transparent;
        pointer-events: none;
        z-index: 1;
      }
      .time-block {
        position: absolute;
        /* Above the forecast band (0) and desired ghost (1), below the
           single-event bars (4) so their existing precedence is unchanged. */
        z-index: 2;
        top: 3px;
        bottom: 3px;
        background: linear-gradient(135deg, var(--block-gradient-start), var(--block-gradient-end));
        border-radius: 4px;
        cursor: pointer;
        transition: filter 0.12s, box-shadow 0.12s;
        min-width: 4px;
        touch-action: none;
        box-shadow: 0 1px 3px rgba(0,0,0,0.15);
      }
      .time-block:hover {
        filter: brightness(1.1);
        box-shadow: 0 2px 6px rgba(0,0,0,0.2);
      }
      .time-block.selected {
        background: linear-gradient(135deg, #ffa726, #f57c00);
        box-shadow: 0 0 0 2px var(--block-selected), 0 2px 8px rgba(255,152,0,0.35);
      }
      .time-block.pending {
        background: repeating-linear-gradient(
          -45deg,
          var(--block-gradient-start),
          var(--block-gradient-start) 4px,
          var(--pending-stripe) 4px,
          var(--pending-stripe) 8px
        );
      }
      .time-block.selected.pending {
        background: repeating-linear-gradient(
          -45deg,
          #ffa726,
          #ffa726 4px,
          #fff176 4px,
          #fff176 8px
        );
        box-shadow: 0 0 0 2px var(--block-selected), 0 2px 8px rgba(255,152,0,0.35);
      }

      /* ─ Drag Handles ─ */
      .drag-handle {
        position: absolute;
        top: -2px;
        bottom: -2px;
        width: 18px;
        cursor: col-resize;
        z-index: 10;
        touch-action: none;
        opacity: 0;
        transition: opacity 0.15s;
      }
      .time-block:hover .drag-handle,
      .time-block.selected .drag-handle {
        opacity: 1;
      }
      .drag-handle.start { left: -6px; }
      .drag-handle.end { right: -6px; }
      .drag-handle::after {
        content: '';
        position: absolute;
        top: 50%;
        left: 50%;
        transform: translate(-50%, -50%);
        width: 3px;
        height: 14px;
        background: rgba(255,255,255,0.8);
        border-radius: 2px;
        box-shadow: 0 0 2px rgba(0,0,0,0.2);
      }

      /* ─ Tooltip ─ */
      .block-tooltip {
        position: absolute;
        top: -28px;
        left: 50%;
        transform: translateX(-50%);
        background: var(--text-color);
        color: var(--card-bg);
        font-size: 0.72em;
        font-weight: 500;
        padding: 3px 8px;
        border-radius: 4px;
        white-space: nowrap;
        pointer-events: none;
        z-index: 25;
        box-shadow: 0 2px 6px rgba(0,0,0,0.2);
        letter-spacing: 0.03em;
      }
      .block-tooltip::after {
        content: '';
        position: absolute;
        top: 100%;
        left: 50%;
        transform: translateX(-50%);
        border: 4px solid transparent;
        border-top-color: var(--text-color);
      }

      /* ─ Drag Tooltip (floating) ─ */
      .drag-tooltip {
        position: fixed;
        background: var(--text-color);
        color: var(--card-bg);
        font-size: 0.78em;
        font-weight: 600;
        padding: 4px 10px;
        border-radius: 6px;
        pointer-events: none;
        z-index: 1000;
        box-shadow: 0 4px 12px rgba(0,0,0,0.3);
        letter-spacing: 0.02em;
        transition: none;
      }

      /* ─ Selection Panel ─ */
      .selection-panel {
        margin-top: 10px;
        padding: 10px 14px;
        background: var(--bg-bar);
        border-radius: var(--radius);
        border: 1px solid var(--divider-color, #e0e0e0);
        animation: slideDown 0.15s ease-out;
      }
      @keyframes slideDown {
        from { opacity: 0; transform: translateY(-6px); }
        to { opacity: 1; transform: translateY(0); }
      }
      .sel-header {
        display: flex;
        align-items: center;
        gap: 10px;
        flex-wrap: wrap;
      }
      .sel-day {
        font-weight: 600;
        font-size: 0.9em;
        color: var(--text-color);
      }
      .sel-hint {
        font-size: 0.85em;
        color: var(--text-secondary);
      }
      .sel-actions {
        display: flex;
        gap: 6px;
        margin-left: auto;
      }
      .time-display {
        display: flex;
        align-items: center;
        background: var(--card-bg);
        border: 1px solid var(--divider-color, #e0e0e0);
        border-radius: 6px;
        padding: 4px 10px;
        font-size: 0.88em;
        font-weight: 500;
        color: var(--text-color);
        cursor: pointer;
        transition: border-color 0.15s, box-shadow 0.15s;
        font-family: inherit;
      }
      .time-display:hover {
        border-color: var(--primary-color);
        box-shadow: 0 0 0 1px var(--primary-color);
      }

      /* ─ Time Editor ─ */
      .time-editor {
        display: flex;
        align-items: center;
        gap: 2px;
      }
      .time-sel {
        appearance: none;
        -webkit-appearance: none;
        background: var(--card-bg);
        border: 1px solid var(--divider-color, #e0e0e0);
        border-radius: 4px;
        padding: 4px 6px;
        font-size: 0.85em;
        font-weight: 500;
        color: var(--text-color);
        cursor: pointer;
        text-align: center;
        min-width: 42px;
        font-family: inherit;
      }
      .time-sel:focus {
        outline: none;
        border-color: var(--primary-color);
        box-shadow: 0 0 0 1px var(--primary-color);
      }
      .time-colon, .time-dash {
        font-weight: 600;
        color: var(--text-secondary);
        padding: 0 2px;
      }

      /* ─ Apply To ─ */
      .apply-to-section {
        margin-top: 10px;
        padding-top: 10px;
        border-top: 1px solid var(--divider-color, #e0e0e0);
        animation: slideDown 0.15s ease-out;
      }
      .apply-to-label {
        font-size: 0.8em;
        color: var(--text-secondary);
        margin-bottom: 6px;
      }
      .apply-to-days {
        display: flex;
        gap: 4px;
        flex-wrap: wrap;
        margin-bottom: 8px;
      }
      .day-check {
        display: flex;
        align-items: center;
        gap: 3px;
        padding: 4px 10px;
        border: 1px solid var(--divider-color, #e0e0e0);
        border-radius: 6px;
        font-size: 0.8em;
        cursor: pointer;
        transition: all 0.12s;
        user-select: none;
        background: var(--card-bg);
        color: var(--text-secondary);
      }
      .day-check:hover {
        border-color: var(--primary-color);
      }
      .day-check.checked {
        background: color-mix(in srgb, var(--primary-color) 12%, transparent);
        border-color: var(--primary-color);
        color: var(--primary-color);
        font-weight: 600;
      }
      .day-check input {
        display: none;
      }
      .apply-to-actions {
        display: flex;
        gap: 6px;
        align-items: center;
      }

      /* ─ Footer ─ */
      .footer {
        display: flex;
        justify-content: space-between;
        align-items: center;
        margin-top: 14px;
        padding-top: 10px;
        border-top: 1px solid var(--divider-color, #e0e0e0);
      }
      .status {
        display: flex;
        align-items: center;
        gap: 8px;
      }
      .status-dot {
        width: 8px;
        height: 8px;
        border-radius: 50%;
        background: #9e9e9e;
        transition: background 0.2s;
      }
      .status-dot.active {
        background: #4caf50;
        box-shadow: 0 0 6px rgba(76,175,80,0.4);
      }
      .actions {
        display: flex;
        align-items: center;
        gap: 6px;
      }

      /* ─ Buttons ─ */
      .btn {
        padding: 6px 14px;
        border: none;
        border-radius: 6px;
        font-size: 0.8em;
        font-weight: 500;
        cursor: pointer;
        transition: all 0.12s;
        display: inline-flex;
        align-items: center;
        justify-content: center;
        font-family: inherit;
      }
      .btn-sm { padding: 4px 10px; font-size: 0.78em; }
      .btn-primary {
        background: var(--primary-color);
        color: white;
      }
      .btn-primary:hover { filter: brightness(0.9); }
      .btn-primary:disabled { opacity: 0.35; cursor: default; filter: none; }
      .btn-outline {
        background: transparent;
        border: 1px solid var(--divider-color, #e0e0e0);
        color: var(--text-secondary);
      }
      .btn-outline:hover { background: var(--bg-bar); }
      .btn-danger {
        background: transparent;
        border: 1px solid var(--danger);
        color: var(--danger);
      }
      .btn-danger:hover { background: color-mix(in srgb, var(--danger) 8%, transparent); }
      .btn-icon {
        background: transparent;
        border: 1px solid var(--divider-color, #e0e0e0);
        padding: 5px 8px;
        border-radius: 6px;
        color: var(--text-secondary);
      }
      .btn-icon:hover { background: var(--bg-bar); }
      .btn-save {
        animation: pulse-save 1.5s ease-in-out infinite;
      }
      @keyframes pulse-save {
        0%, 100% { box-shadow: none; }
        50% { box-shadow: 0 0 8px rgba(79,195,247,0.5); }
      }

      /* ─ Quick Run ─ */
      .quick-run-chip {
        display: inline-flex;
        align-items: center;
        padding: 3px 10px;
        border-radius: 14px;
        font-size: 0.76em;
        font-weight: 600;
        border: 1px solid var(--qr-color);
        color: var(--qr-color);
        background: transparent;
        cursor: pointer;
        transition: all 0.15s;
        font-family: inherit;
      }
      .quick-run-chip:hover {
        background: color-mix(in srgb, var(--qr-color) 10%, transparent);
      }
      .quick-run-chip.active {
        background: var(--qr-color);
        color: white;
      }
      .quick-run-panel {
        margin-top: 10px;
        padding: 12px 14px;
        background: color-mix(in srgb, var(--qr-color) 6%, var(--bg-bar));
        border-radius: var(--radius);
        border: 1px solid color-mix(in srgb, var(--qr-color) 30%, var(--divider-color, #e0e0e0));
        animation: slideDown 0.15s ease-out;
      }
      .qr-header {
        display: flex;
        align-items: center;
        gap: 6px;
        margin-bottom: 10px;
      }
      .qr-title {
        font-weight: 600;
        font-size: 0.9em;
        color: var(--text-color);
      }
      .qr-subtitle {
        font-size: 0.78em;
        color: var(--text-secondary);
        margin-left: auto;
      }
      .qr-presets {
        display: flex;
        gap: 6px;
        flex-wrap: wrap;
      }
      .qr-preset {
        padding: 6px 16px;
        border: 1px solid color-mix(in srgb, var(--qr-color) 40%, var(--divider-color, #e0e0e0));
        border-radius: 8px;
        background: var(--card-bg);
        color: var(--text-color);
        font-size: 0.84em;
        font-weight: 600;
        cursor: pointer;
        transition: all 0.12s;
        font-family: inherit;
        display: inline-flex;
        align-items: center;
      }
      .qr-preset:hover {
        border-color: var(--qr-color);
        background: color-mix(in srgb, var(--qr-color) 8%, var(--card-bg));
      }
      .qr-preset:active {
        background: var(--qr-color);
        color: white;
      }
      .qr-preset.active {
        background: color-mix(in srgb, var(--qr-color) 12%, var(--card-bg));
        border-color: var(--qr-color);
      }
      .qr-custom-section {
        margin-top: 10px;
        padding-top: 10px;
        border-top: 1px solid var(--divider-color, #e0e0e0);
        animation: slideDown 0.15s ease-out;
      }
      .qr-custom-row {
        display: flex;
        align-items: center;
        gap: 6px;
        margin-bottom: 6px;
      }
      .qr-custom-label {
        font-size: 0.8em;
        font-weight: 500;
        color: var(--text-secondary);
        width: 36px;
        flex-shrink: 0;
      }
      .qr-date-input {
        appearance: none;
        -webkit-appearance: none;
        background: var(--card-bg);
        border: 1px solid var(--divider-color, #e0e0e0);
        border-radius: 4px;
        padding: 4px 8px;
        font-size: 0.82em;
        color: var(--text-color);
        font-family: inherit;
      }
      .qr-date-input:focus {
        outline: none;
        border-color: var(--qr-color);
        box-shadow: 0 0 0 1px var(--qr-color);
      }
      .qr-events {
        margin-top: 10px;
        padding-top: 10px;
        border-top: 1px solid var(--divider-color, #e0e0e0);
      }
      .qr-events-label {
        font-size: 0.78em;
        color: var(--text-secondary);
        margin-bottom: 6px;
      }
      .qr-event-row {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: 4px 8px;
        margin-bottom: 3px;
        background: var(--card-bg);
        border-radius: 6px;
        border: 1px solid var(--divider-color, #e0e0e0);
      }
      .qr-event-time {
        font-size: 0.84em;
        font-weight: 500;
        color: var(--text-color);
        font-variant-numeric: tabular-nums;
      }
      .qr-event-clear {
        background: transparent;
        border: none;
        color: var(--danger);
        cursor: pointer;
        padding: 2px;
        border-radius: 50%;
        display: flex;
        align-items: center;
        transition: background 0.12s;
      }
      .qr-event-clear:hover {
        background: color-mix(in srgb, var(--danger) 10%, transparent);
      }
    `;
  }

  /* ─── Events ─── */

  _attachEvents() {
    const root = this.shadowRoot;

    // Timeline bar clicks
    root.querySelectorAll('.timeline-bar').forEach(bar => {
      bar.addEventListener('click', (e) => {
        if (e.target.classList.contains('drag-handle')) return;
        const day = parseInt(bar.dataset.day);

        if (e.target.closest('.time-block')) {
          const block = e.target.closest('.time-block');
          const layer = parseInt(block.dataset.layer);
          const isSame = this._selectedBlock &&
            this._selectedBlock.day === day && this._selectedBlock.layer === layer;
          this._selectedBlock = isSame ? null : { day, layer };
          this._showApplyTo = false;
          this._applyDays.clear();
          this._editingTime = false;
          this._render();
          return;
        }

        // Click on empty bar — add a new block
        const rect = bar.getBoundingClientRect();
        const x = e.clientX - rect.left;
        const clickMins = this._snapMinutes(Math.round((x / rect.width) * MINUTES_IN_DAY));
        const freeLayer = this._findFreeLayer(day);
        if (freeLayer >= 0) {
          const start = Math.max(0, clickMins - 60);
          const end = Math.min(MINUTES_IN_DAY, clickMins + 60);
          this._pendingChanges.set(`${day},${freeLayer}`, [start, end]);
          this._selectedBlock = { day, layer: freeLayer };
          this._showApplyTo = false;
          this._editingTime = false;
        }
        this._render();
      });
    });

    // Block hover for tooltips
    root.querySelectorAll('.time-block').forEach(block => {
      block.addEventListener('mouseenter', () => {
        const day = parseInt(block.dataset.day);
        const layer = parseInt(block.dataset.layer);
        this._hoverBlock = { day, layer };
        // Update just this block's tooltip without full re-render
        const blocks = this._getDayBlocks(day);
        const b = blocks.find(b => b.layer === layer);
        if (b) {
          let tooltip = block.querySelector('.block-tooltip');
          if (!tooltip) {
            tooltip = document.createElement('div');
            tooltip.className = 'block-tooltip';
            block.appendChild(tooltip);
          }
          tooltip.textContent = `${this._minutesToTime(b.start)} – ${this._minutesToTime(b.end)}`;
        }
      });
      block.addEventListener('mouseleave', () => {
        this._hoverBlock = null;
        const tooltip = block.querySelector('.block-tooltip');
        const sel = this._selectedBlock &&
          parseInt(block.dataset.day) === this._selectedBlock.day &&
          parseInt(block.dataset.layer) === this._selectedBlock.layer;
        if (tooltip && !sel) tooltip.remove();
      });
    });

    // Drag handles
    root.querySelectorAll('.drag-handle').forEach(handle => {
      handle.addEventListener('pointerdown', (e) => {
        e.preventDefault();
        e.stopPropagation();
        handle.setPointerCapture(e.pointerId);
        const day = parseInt(handle.dataset.day);
        const layer = parseInt(handle.dataset.layer);
        const edge = handle.dataset.edge;
        const bar = handle.closest('.timeline-bar');
        const rect = bar.getBoundingClientRect();
        const block = handle.closest('.time-block');

        this._selectedBlock = { day, layer };

        const data = this._getDayBlocks(day);
        const entry = data.find(b => b.layer === layer);
        if (!entry) return;
        let curStart = entry.start, curEnd = entry.end;

        // Create floating drag tooltip
        const dragTip = document.createElement('div');
        dragTip.className = 'drag-tooltip';
        dragTip.style.cssText = `position:fixed;z-index:10000;`;
        document.body.appendChild(dragTip);

        const updateDragTip = (me) => {
          dragTip.textContent = edge === 'start' ? this._minutesToTime(curStart) : this._minutesToTime(curEnd);
          dragTip.style.left = `${me.clientX + 12}px`;
          dragTip.style.top = `${me.clientY - 30}px`;
        };
        updateDragTip(e);

        const onMove = (me) => {
          const x = Math.max(0, Math.min(me.clientX - rect.left, rect.width));
          const mins = this._snapMinutes(Math.round((x / rect.width) * MINUTES_IN_DAY));

          if (edge === 'start') {
            curStart = Math.max(0, Math.min(mins, curEnd - MIN_BLOCK_MINUTES));
          } else {
            curEnd = Math.min(MINUTES_IN_DAY, Math.max(mins, curStart + MIN_BLOCK_MINUTES));
          }

          // Direct DOM update
          const leftPct = (curStart / MINUTES_IN_DAY) * 100;
          const widthPct = ((curEnd - curStart) / MINUTES_IN_DAY) * 100;
          block.style.left = leftPct + '%';
          block.style.width = widthPct + '%';

          // Update block tooltip
          const tooltip = block.querySelector('.block-tooltip');
          if (tooltip) tooltip.textContent = `${this._minutesToTime(curStart)} – ${this._minutesToTime(curEnd)}`;

          // Update selection panel time
          const timeBtn = root.querySelector('.time-display');
          if (timeBtn) timeBtn.childNodes[0].textContent = `${this._minutesToTime(curStart)} – ${this._minutesToTime(curEnd)} `;

          updateDragTip(me);
        };

        const onUp = () => {
          document.removeEventListener('pointermove', onMove);
          document.removeEventListener('pointerup', onUp);
          if (dragTip.parentNode) dragTip.remove();
          this._pendingChanges.set(`${day},${layer}`, [curStart, curEnd]);
          this._render();
        };

        document.addEventListener('pointermove', onMove);
        document.addEventListener('pointerup', onUp);
      });
    });

    // Action buttons
    root.querySelectorAll('[data-action]').forEach(btn => {
      btn.addEventListener('click', (e) => {
        e.stopPropagation();
        const action = btn.dataset.action;
        switch (action) {
          case 'refresh': this._callRefresh(); break;
          case 'save': this._saveChanges(); break;
          case 'discard': this._discardChanges(); break;
          case 'add': this._addBlock(parseInt(btn.dataset.day)); break;
          case 'delete':
            this._deleteBlock(parseInt(btn.dataset.day), parseInt(btn.dataset.layer));
            break;
          case 'toggle-schedule': this._toggleSchedule(); break;
          case 'edit-time':
            this._editingTime = true;
            this._render();
            break;
          case 'save-time':
            this._saveInlineTime();
            break;
          case 'apply-to':
            this._showApplyTo = !this._showApplyTo;
            this._applyDays.clear();
            this._render();
            break;
          case 'select-weekdays':
            this._applyDays.clear();
            for (let i = 0; i < 5; i++) {
              if (this._selectedBlock && i !== this._selectedBlock.day) this._applyDays.add(i);
            }
            this._render();
            break;
          case 'select-all-days':
            this._applyDays.clear();
            for (let i = 0; i < 7; i++) {
              if (this._selectedBlock && i !== this._selectedBlock.day) this._applyDays.add(i);
            }
            this._render();
            break;
          case 'confirm-apply':
            this._applyToSelectedDays();
            break;
          case 'toggle-quick-run':
            this._showQuickRun = !this._showQuickRun;
            this._quickRunCustom = false;
            if (this._showQuickRun) {
              // Refresh single events when opening
              this._callRefreshSingleEvents();
            }
            this._render();
            break;
          case 'quick-run-preset':
            this._scheduleQuickRun(parseInt(btn.dataset.minutes));
            break;
          case 'toggle-quick-run-custom':
            this._quickRunCustom = !this._quickRunCustom;
            this._render();
            break;
          case 'schedule-custom-run':
            this._scheduleCustomRun();
            break;
          case 'clear-single-event':
            this._clearSingleEvent(parseInt(btn.dataset.slot));
            break;
        }
      });
    });

    // Apply-to checkboxes
    root.querySelectorAll('[data-apply-day]').forEach(cb => {
      cb.addEventListener('change', () => {
        const day = parseInt(cb.dataset.applyDay);
        if (cb.checked) this._applyDays.add(day);
        else this._applyDays.delete(day);
        this._render();
      });
    });
  }

  /* ─── Actions ─── */

  _addBlock(day) {
    const freeLayer = this._findFreeLayer(day);
    if (freeLayer < 0) return;
    this._pendingChanges.set(`${day},${freeLayer}`, [360, 480]); // 06:00 – 08:00
    this._selectedBlock = { day, layer: freeLayer };
    this._showApplyTo = false;
    this._editingTime = false;
    this._render();
  }

  _deleteBlock(day, layer) {
    this._pendingChanges.set(`${day},${layer}`, null);
    this._selectedBlock = null;
    this._showApplyTo = false;
    this._render();
  }

  _saveInlineTime() {
    if (!this._selectedBlock) return;
    const root = this.shadowRoot;
    const sH = parseInt(root.querySelector('[data-field="startH"]').value);
    const sM = parseInt(root.querySelector('[data-field="startM"]').value);
    const eH = parseInt(root.querySelector('[data-field="endH"]').value);
    const eM = parseInt(root.querySelector('[data-field="endM"]').value);
    const start = sH * 60 + sM;
    const end = eH * 60 + eM;
    if (end <= start) return; // invalid range
    const { day, layer } = this._selectedBlock;
    this._pendingChanges.set(`${day},${layer}`, [start, end]);
    this._editingTime = false;
    this._render();
  }

  _applyToSelectedDays() {
    if (!this._selectedBlock) return;
    const { day: srcDay, layer: srcLayer } = this._selectedBlock;
    const blocks = this._getDayBlocks(srcDay);
    const srcBlock = blocks.find(b => b.layer === srcLayer);
    if (!srcBlock) return;

    for (const targetDay of this._applyDays) {
      const freeLayer = this._findFreeLayer(targetDay);
      if (freeLayer >= 0) {
        this._pendingChanges.set(`${targetDay},${freeLayer}`, [srcBlock.start, srcBlock.end]);
      }
    }
    this._showApplyTo = false;
    this._applyDays.clear();
    this._render();
  }

  _discardChanges() {
    if (this._isSaving()) return; // writes are already out; let them settle
    this._writeErrors = [];
    this._pendingChanges.clear();
    this._selectedBlock = null;
    this._showApplyTo = false;
    this._editingTime = false;
    this._render();
  }

  /**
   * Call an alpha_hwr service.
   *
   * Every service registered by api_bridge.cpp declares an `op_id` argument,
   * and Home Assistant registers user-defined ESPHome service arguments as
   * vol.Required -- so a call omitting it is rejected before it ever reaches
   * the device. The card previously omitted it on every call, which made every
   * write and both refreshes silent no-ops. The id also correlates the
   * resulting esphome.alpha_hwr_write_settled event back to this action.
   *
   * The rejection surfaces only as an unhandled promise rejection, so failures
   * were invisible; catch and log them.
   *
   * `track`, when given, registers the call in _inFlight under its op_id so
   * the write is not treated as done until the device says it is. A call HA
   * itself rejects never produces a settle event, so drop it here too.
   */
  _callService(service, data = {}, track = null) {
    const device = this._config.device;
    this._opSeq = (this._opSeq || 0) + 1;
    const opId = `card-${Date.now()}-${this._opSeq}`;
    if (track) this._inFlight.set(opId, track);
    return this._hass
      .callService('esphome', `${device}_${service}`, { ...data, op_id: opId })
      .catch((err) => {
        console.error(`[alpha-hwr-card] ${service} failed:`, err);
        if (track && this._inFlight.delete(opId)) {
          this._writeErrors.push({
            surface: track.surface, label: track.label,
            status: 'call rejected', detail: String(err),
          });
          this._afterSettle();
        }
      });
  }

  _saveChanges() {
    // One save at a time. Re-issuing writes for edits already in flight would
    // put two op_ids on one cell, and the loser's settle would resolve an edit
    // the winner is still carrying.
    if (this._inFlight.size > 0) return;

    const batch = [...this._pendingChanges];
    if (batch.length === 0) return;
    // Clear only this surface's errors. These writes are the retry for them; a
    // Quick Run is not, and must not wipe a schedule failure the user has not
    // read yet.
    this._writeErrors = this._writeErrors.filter(e => e.surface !== 'schedule');
    this._refreshOnDrain.add('schedule');

    for (const [key, entry] of batch) {
      const [day, layer] = key.split(',').map(Number);
      const track = { surface: 'schedule', key, entry, label: `${DAYS[day]} layer ${layer}` };
      if (entry === null) {
        this._callService('clear_schedule_entry', { data: `${layer},${day}` }, track);
      } else {
        const [start, end] = entry;
        // A block dragged to the right edge yields end === 1440, which
        // serialises as hour 24 and api_bridge.cpp rejects (vals[4] > 23), so
        // "run until midnight" could never be saved. 23:59 is the pump's
        // end-of-day representation.
        const endClamped = Math.min(end, 1439);
        const sh = Math.floor(start / 60);
        const sm = start % 60;
        const eh = Math.floor(endClamped / 60);
        const em = endClamped % 60;
        this._callService('set_schedule_entry', {
          data: `${layer},${day},${sh},${sm},${eh},${em}`,
        }, track);
      }
    }

    // The edits stay in _pendingChanges until their own settle arrives; the
    // grid keeps showing what the user asked for rather than snapping back.
    this._selectedBlock = null;
    this._showApplyTo = false;
    this._editingTime = false;

    this._backstopUnitMs = WATCHDOG_SCHED_ENTRY_MS;
    this._armBackstopForInFlight();
    this._render();
  }

  _toggleSchedule() {
    const enabled = this._schedule ? this._schedule.e : 0;
    // Toggle the Schedule Enabled *switch* (not the raw set_schedule_enabled
    // service) so the card gets the switch's coupled behavior: enabling forces
    // the pump to AUTO so the schedule can actually run it (a stopped pump with
    // the schedule enabled never runs), and disabling stops the pump. This
    // keeps the card consistent with the Engage Pump / Schedule Enabled
    // mutual-exclusion model.
    this._hass.callService('switch', enabled ? 'turn_off' : 'turn_on', {
      entity_id: this._config.enabled_entity,
    });
    setTimeout(() => this._callRefresh(), 2000);
  }

  _callRefresh() {
    this._callService('refresh_schedule');
  }

  /* ─── Quick Run Actions ─── */

  _scheduleQuickRun(durationMinutes) {
    const now = Math.floor(Date.now() / 1000);
    const begin = now + 60; // start 1 minute from now
    const end = begin + durationMinutes * 60;
    this._trackSingleEvent('set_single_event', { data: `${begin},${end}` },
                           `Quick Run ${durationMinutes} min`);
  }

  _scheduleCustomRun() {
    const root = this.shadowRoot;
    const startDate = root.querySelector('[data-field="qr-start-date"]')?.value;
    const startH = parseInt(root.querySelector('[data-field="qrStartH"]')?.value || '0');
    const startM = parseInt(root.querySelector('[data-field="qrStartM"]')?.value || '0');
    const endDate = root.querySelector('[data-field="qr-end-date"]')?.value;
    const endH = parseInt(root.querySelector('[data-field="qrEndH"]')?.value || '0');
    const endM = parseInt(root.querySelector('[data-field="qrEndM"]')?.value || '0');

    if (!startDate || !endDate) return;

    const [sy, smo, sd] = startDate.split('-').map(Number);
    const [ey, emo, ed] = endDate.split('-').map(Number);
    const beginTs = Math.floor(new Date(sy, smo - 1, sd, startH, startM).getTime() / 1000);
    const endTs = Math.floor(new Date(ey, emo - 1, ed, endH, endM).getTime() / 1000);

    if (endTs <= beginTs) return;

    this._quickRunCustom = false;
    this._trackSingleEvent('set_single_event', { data: `${beginTs},${endTs}` },
                           'Custom run');
  }

  _clearSingleEvent(slot) {
    // No optimistic removal: a single-event write carries a 60 s watchdog, so
    // a row deleted on faith would sit gone for up to a minute and then
    // reappear if the write failed. Mark it pending and let the settle decide.
    this._trackSingleEvent('clear_single_event', { data: `${slot}` }, `Event ${slot}`,
                           { clearedSlot: slot });
  }

  /** Issue a single-event write and wait for its settle, like _saveChanges. */
  _trackSingleEvent(service, data, label, extra = {}) {
    // One outstanding write at a time: the backstop is a single timer, so a
    // second op would either inherit the first's deadline or overwrite it. The
    // constraint is rendered -- every button that reaches here is disabled
    // while _isSaving() -- so this guard is a backstop against a stale click,
    // not the mechanism the user sees.
    if (this._inFlight.size > 0) return;
    this._writeErrors = this._writeErrors.filter(e => e.surface !== 'single');
    this._refreshOnDrain.add('single');
    this._callService(service, data, { surface: 'single', label, ...extra });
    this._backstopUnitMs = WATCHDOG_SINGLE_EVENT_MS;
    this._armBackstopForInFlight();
    this._render();
  }

  _callRefreshSingleEvents() {
    this._callService('refresh_single_events');
  }

  getCardSize() {
    return 5;
  }

  static getStubConfig() {
    return {
      device: 'hwr_pump',
      title: 'Pump Schedule',
    };
  }
}

customElements.define('alpha-hwr-schedule-card', AlphaHwrScheduleCard);

window.customCards = window.customCards || [];
window.customCards.push({
  type: 'alpha-hwr-schedule-card',
  name: 'Alpha HWR Schedule',
  description: 'Visual weekly schedule editor for Grundfos ALPHA HWR pump',
  preview: true,
});
