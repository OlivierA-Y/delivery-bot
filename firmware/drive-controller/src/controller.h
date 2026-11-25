#pragma once

#include <Arduino.h>

const char kControllerPage[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#edf0ec">
<title>HotelBot Drive</title>
<style>
  :root {
    color-scheme: light;
    --paper: #edf0ec;
    --ink: #252824;
    --muted: #50554b;
    --pad: #dce2d9;
    --travel: #cbd5c6;
    --active: #cfed76;
    --stop: #a72b26;
    font: 16px/1.45 system-ui, sans-serif;
    background: var(--paper);
    color: var(--ink);
  }
  * { box-sizing: border-box; }
  body {
    margin: 0;
    min-height: 100svh;
    display: grid;
    align-items: center;
    padding: max(20px, env(safe-area-inset-top)) 20px max(20px, env(safe-area-inset-bottom));
  }
  main { width: min(100%, 57rem); margin-inline: auto; }
  h1 { margin: 0; font-size: clamp(3rem, 9vw, 5.5rem); line-height: .93; letter-spacing: -.06em; }
  p { margin: 12px 0 0; }
  #status { color: var(--muted); min-height: 1.5em; font-weight: 680; }
  .guide { color: var(--muted); }
  .wheel-speeds { display: flex; gap: 25px; margin: 20px 0 0; }
  .wheel-speeds dt { color: var(--muted); }
  .wheel-speeds dd { margin: 0; font-size: 1.25rem; font-weight: 720; font-variant-numeric: tabular-nums; }
  .reading-label { margin-top: 2px; color: var(--muted); font-size: .875rem; }
  .controls { display: grid; gap: 14px; margin-top: 18px; }
  button { font: inherit; color: inherit; border: 0; cursor: pointer; }
  button:focus-visible, input:focus-visible { outline: 3px solid var(--ink); outline-offset: 4px; }
  #stick {
    width: min(100%, 16rem, 38svh);
    aspect-ratio: 1;
    margin-inline: auto;
    display: grid;
    place-items: center;
    position: relative;
    padding: 0;
    border-radius: 50%;
    background: var(--pad);
    touch-action: none;
    user-select: none;
    -webkit-user-select: none;
    -webkit-touch-callout: none;
  }
  #stick:disabled { opacity: .58; cursor: default; }
  #stick::before { content: ""; width: 68%; aspect-ratio: 1; border-radius: 50%; background: var(--travel); }
  #stick span { position: absolute; pointer-events: none; font-size: 1.15rem; line-height: 1; }
  .up { top: 7%; } .down { bottom: 7%; } .left { left: 7%; } .right { right: 7%; }
  #knob { width: 31%; aspect-ratio: 1; border-radius: 50%; background: var(--ink); }
  #stick[data-held] #knob { background: var(--active); }
  .speed { display: grid; grid-template-columns: 1fr auto; gap: 2px 10px; font-weight: 680; }
  output { font-variant-numeric: tabular-nums; }
  input[type=range] { grid-column: span 2; width: 100%; height: 34px; margin: 0; accent-color: var(--ink); }
  #stop { min-height: 50px; background: var(--stop); color: white; font-weight: 720; }
  #stop:active { background: var(--ink); }
  #override { min-height: 48px; background: var(--pad); font-weight: 720; }
  #override[hidden] { display: none; }
  #override:disabled { opacity: .58; }
  button:disabled { cursor: default; opacity: .58; }
  h2 { margin: 0; font-size: 1.25rem; letter-spacing: -.02em; }
  .direct { margin-top: 24px; }
  .numbers { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 10px; }
  .numbers label, .gains label { display: grid; gap: 4px; font-size: .875rem; }
  input[type=number] {
    width: 100%; min-height: 42px; padding: 7px 9px; border: 1px solid #929a8c;
    background: #f9faf7; border-radius: 0; color: inherit; font: inherit; font-variant-numeric: tabular-nums;
  }
  #run, .tune button { min-height: 42px; padding: 8px 14px; background: var(--pad); font-weight: 680; }
  #run { width: 100%; margin-top: 10px; touch-action: none; user-select: none; -webkit-touch-callout: none; }
  #run[data-held] { background: var(--active); }
  .panel { grid-column: 1 / -1; margin-top: 28px; padding-top: 18px; border-top: 1px solid #adb5a7; }
  .graph-head { display: flex; justify-content: space-between; align-items: center; gap: 12px; flex-wrap: wrap; }
  .graph-actions { display: flex; flex-wrap: wrap; gap: 8px; }
  .wheel-pick { display: flex; gap: 4px; }
  .graph-actions button { min-height: 38px; padding-inline: 14px; background: var(--pad); }
  .wheel-pick button[aria-pressed=true] { background: var(--ink); color: white; }
  .charts { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 14px 24px; margin-top: 14px; }
  figure { margin: 0; min-width: 0; }
  figcaption { font-weight: 700; }
  canvas { width: 100%; height: 142px; display: block; }
  .legend { display: flex; flex-wrap: wrap; gap: 3px 12px; min-height: 1.4em; color: var(--muted); font-size: .8rem; }
  .legend span::before { content: ""; display: inline-block; width: 12px; margin: 0 5px 3px 0; border-top: 2px solid var(--line); }
  .tune { display: grid; gap: 12px; margin-top: 24px; }
  .gain-rows { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 16px; }
  .gains { min-width: 0; margin: 0; padding: 8px 10px 10px; border: 1px solid #adb5a7; }
  .gains legend { padding-inline: 4px; font-weight: 700; }
  .gain-inputs { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 8px; margin-bottom: 10px; }
  .tune-actions { display: flex; flex-wrap: wrap; gap: 8px; }
  #tune-status { margin: 0; min-height: 1.4em; color: var(--muted); }
  @media (max-width: 570px) { .charts, .gain-rows { grid-template-columns: 1fr; } }
  @media (min-width: 670px), (orientation: landscape) and (min-width: 530px) {
    main { display: grid; grid-template-columns: 1fr minmax(240px, 22rem); align-items: center; gap: 25px; }
    .controls { margin: 0; }
  }
  @media (max-height: 480px) and (orientation: landscape) {
    h1 { font-size: 2.7rem; }
    .controls { gap: 8px; }
    #stop { min-height: 44px; }
  }
</style>
<script>
document.addEventListener('DOMContentLoaded', () => {
  const stick = document.querySelector('#stick');
  const knob = document.querySelector('#knob');
  const status = document.querySelector('#status');
  const limit = document.querySelector('#limit');
  const limitValue = document.querySelector('#limit-value');
  const stop = document.querySelector('#stop');
  const overrideButton = document.querySelector('#override');
  const leftRpm = document.querySelector('#left-rpm');
  const rightRpm = document.querySelector('#right-rpm');
  const setpoints = [...document.querySelectorAll('.numbers input')];
  const run = document.querySelector('#run');
  const normalMax = Number(limit.max);
  const tuneStatus = document.querySelector('#tune-status');
  const tuneButtons = [...document.querySelectorAll('[data-tune]')];
  const gainInputs = [...document.querySelectorAll('.gain-inputs')].map(row => [...row.querySelectorAll('input')]);
  const graphButtons = [...document.querySelectorAll('[data-wheel]')];
  const pauseGraphs = document.querySelector('#pause-graphs');
  const charts = [...document.querySelectorAll('canvas')];
  const graphSeconds = 12;
  const history = [];
  let pausedHistory = null;
  let graphWheel = 'left';
  let lastState = null;
  let wheelReady = false;
  let direct = null;
  let runKey = null;
  let tuningRequest = null;
  let gainsLoaded = false;
  const arrowKeys = ['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight'];
  const keys = new Set();
  let session = 0;
  let sequence = 0;
  let pointer = null;
  let held = false;
  let motion = null;
  let outstanding = 0;
  let lastDriveReplyAt = 0;
  let polling = false;
  let connecting = false;
  let x = 0;
  let y = 0;
  let fault = 0;
  let overridePending = false;
  let driveNotice = '';

  function directTargets() {
    return setpoints.map(input => input.checkValidity() ? input.valueAsNumber : NaN);
  }

  function updateButtons() {
    const ready = Boolean(session && motion?.readyState === WebSocket.OPEN && wheelReady && !tuningRequest);
    stick.disabled = !ready;
    const speeds = directTargets();
    run.disabled = !ready || speeds.some(value => !Number.isInteger(value) || (value !== 0 && Math.abs(value) < 3)) ||
      speeds.every(value => value === 0);
    for (const input of setpoints) input.readOnly = Boolean(direct);
    for (const button of tuneButtons) {
      button.disabled = !session || !lastState || Boolean(tuningRequest) ||
        (button.dataset.tune !== 'read' && (held || lastState.tuneBusy || lastState.bench ||
          lastState.leftTarget !== 0 || lastState.rightTarget !== 0));
    }
  }

  function readGains() {
    for (const [index, gains] of [lastState.leftGains, lastState.rightGains].entries()) {
      gains.forEach((value, term) => { gainInputs[index][term].value = Number(value.toPrecision(7)); });
    }
    gainsLoaded = true;
  }

  const lines = [
    { keys: ['target', 'estimate'], colors: ['#355a91', '#226344'], floor: 10, unit: 'RPM' },
    { keys: ['error'], colors: ['#8e531c'], floor: 10, unit: 'RPM' },
    { keys: ['p', 'i', 'd'], colors: ['#355a91', '#226344', '#98506f'], floor: 5, unit: 'pp' },
    { keys: ['output'], colors: ['#226344'], floor: 10, unit: '%', positive: true }
  ];

  function drawGraphs() {
    const faultBit = graphWheel === 'left' ? 1 : 2;
    const series = (pausedHistory ?? history).map(state => {
      const target = state[graphWheel + 'Target'];
      const estimate = state.fault & faultBit ? NaN : state[graphWheel];
      const [p, i, d, output] = state[graphWheel + 'Pid'];
      return { ms: state.ms, target, estimate, error: target - estimate, p, i, d, output };
    });
    for (const [index, canvas] of charts.entries()) {
      const width = canvas.clientWidth;
      const height = canvas.clientHeight;
      if (!width || !height) continue;
      const pixelRatio = window.devicePixelRatio || 1;
      const pixelWidth = Math.round(width * pixelRatio);
      const pixelHeight = Math.round(height * pixelRatio);
      if (canvas.width !== pixelWidth || canvas.height !== pixelHeight) {
        canvas.width = pixelWidth;
        canvas.height = pixelHeight;
      }
      const ctx = canvas.getContext('2d');
      ctx.setTransform(pixelRatio, 0, 0, pixelRatio, 0, 0);
      ctx.clearRect(0, 0, width, height);
      const { keys, colors, floor, unit, positive } = lines[index];
      let peak = floor;
      for (const point of series) {
        for (const key of keys) {
          if (Number.isFinite(point[key])) peak = Math.max(peak, Math.abs(point[key]));
        }
      }
      const step = peak < 30 ? 5 : peak < 80 ? 10 : 20;
      const upper = Math.ceil(peak / step) * step;
      const lower = positive ? 0 : -upper;
      const left = 47, top = 12, right = width - 4, bottom = height - 22;
      const end = series.length ? series.at(-1).ms : 0;
      const xAt = ms => right + (ms - end) / (graphSeconds * 1000) * (right - left);
      const yAt = value => bottom - (value - lower) / (upper - lower) * (bottom - top);
      ctx.font = '11px system-ui';
      ctx.fillStyle = '#50554b';
      ctx.textAlign = 'right';
      for (const value of [upper, (upper + lower) / 2, lower]) {
        const y = yAt(value);
        ctx.strokeStyle = value === 0 ? '#929a8c' : '#c8d0c4';
        ctx.beginPath();
        ctx.moveTo(left, y);
        ctx.lineTo(right, y);
        ctx.stroke();
        ctx.fillText(value + (value === upper ? ' ' + unit : ''), left - 5, y + 4);
      }
      ctx.textAlign = 'left';
      ctx.fillText('−' + graphSeconds + ' s', left, height - 4);
      ctx.textAlign = 'right';
      ctx.fillText(pausedHistory ? 'At pause' : 'Now', right, height - 4);
      ctx.save();
      ctx.beginPath();
      ctx.rect(left - 1, top - 2, right - left + 3, bottom - top + 4);
      ctx.clip();
      for (const [line, key] of keys.entries()) {
        ctx.strokeStyle = colors[line];
        ctx.lineWidth = 1.7;
        ctx.beginPath();
        let previous;
        for (const point of series) {
          if (!Number.isFinite(point[key])) { previous = undefined; continue; }
          const x = xAt(point.ms), y = yAt(point[key]);
          if (previous !== undefined && point.ms - previous < 1000) ctx.lineTo(x, y);
          else { ctx.moveTo(x - 1, y); ctx.lineTo(x, y); }
          previous = point.ms;
        }
        ctx.stroke();
      }
      ctx.restore();
    }
  }

  function renderState(state) {
    if (lastState && state.boot !== lastState.boot) {
      history.length = 0;
      gainsLoaded = false;
      driveNotice = state.brownout ? 'Stopped · ESP32 restarted after a voltage drop'
        : 'Stopped · ESP32 restarted';
    }
    lastState = state;
    wheelReady = state.ready;
    if (!gainsLoaded) readGains();
    if (tuningRequest?.id && tuningRequest.id === state.tuneId) {
      const side = tuningRequest.side;
      if (state.tuneOk && side !== 's') {
        const index = side === 'l' ? 0 : 1;
        const values = side === 'l' ? state.leftGains : state.rightGains;
        values.forEach((value, term) => { gainInputs[index][term].value = Number(value.toPrecision(7)); });
      }
      tuneStatus.textContent = !state.tuneOk ? 'Could not change tuning.'
        : side === 's' ? 'Gains saved to the ESP32.'
        : (side === 'l' ? 'Left' : 'Right') + ' gains applied.';
      tuningRequest = null;
    }
    if (history.length && state.ms < history.at(-1).ms) history.length = 0;
    if (!history.length || state.ms !== history.at(-1).ms) {
      history.push(state);
      while (history[0].ms < state.ms - graphSeconds * 1000 || history.length > 150) history.shift();
      if (!pausedHistory) drawGraphs();
    }
    fault = state.fault;
    const hallFault = fault & 3;
    const active = state.override;
    stop.textContent = hallFault && !active ? 'Clear Hall fault' : 'Stop';
    overrideButton.hidden = !hallFault || active;
    overrideButton.disabled = overridePending;
    if (state.max !== Number(limit.max)) {
      limit.max = state.max;
      limitValue.value = limit.value + ' RPM';
    }
    for (const input of setpoints) {
      input.max = state.max;
      input.min = -state.max;
    }
    if (held && !state.ready) release();
    updateButtons();
    leftRpm.textContent = (active && (fault & 1) ? '—' : state.left) + ' / ' + state.leftTarget;
    rightRpm.textContent = (active && (fault & 2) ? '—' : state.right) + ' / ' + state.rightTarget;
    status.textContent = state.bench ? 'USB bench test active'
      : active ? 'Hall override active · limited drive'
      : fault === 3 ? 'Both Hall signals lost'
      : fault === 1 ? 'Left Hall feedback lost'
      : fault === 2 ? 'Right Hall feedback lost'
      : fault === 4 ? 'PWM fault. Restart the ESP32.'
      : state.calibrate ? 'Calibrate both wheels over USB'
      : state.tuneBusy ? 'Applying PID tuning'
      : !state.ready ? 'Drive unavailable'
      : held ? 'Driving' : driveNotice || 'Ready';
  }

  async function connect() {
    if (document.hidden || connecting || session) return;
    connecting = true;
    try {
      const reply = await fetch('/control', {
        method: 'POST', headers: { 'X-Drive': '1' }, cache: 'no-store',
        signal: AbortSignal.timeout(1200)
      });
      if (!reply.ok) throw Error();
      const state = await reply.json();
      session = state.session;
      sequence = 0;
      openMotion();
      renderState(state);
    } catch {
      status.textContent = driveNotice || 'Waiting for HotelBot…';
    } finally {
      connecting = false;
    }
  }

  async function pollStatus() {
    // Motion replies already carry telemetry. Keep a single request path during a hold.
    if (document.hidden || polling || held) return;
    if (!session) return connect();
    const token = session;
    const before = sequence;
    polling = true;
    try {
      const response = await fetch('/status', { cache: 'no-store', signal: AbortSignal.timeout(700) });
      if (!response.ok) throw Error();
      const state = await response.json();
      if (session !== token || sequence !== before) return;
      if (lastState && state.boot !== lastState.boot) disconnectMotion(motion);
      renderState(state);
    } catch {
      if (!held && session === token && sequence === before && !motion) disconnectMotion(motion);
    } finally {
      polling = false;
    }
  }

  function driveQuery(action) {
    const query = new URLSearchParams({
      session, seq: ++sequence, x: x + 100, y: y + 100, rpm: limit.value, clear: action
    });
    if (!action && direct) {
      query.set('direct', '1');
      query.set('left', direct[0] + normalMax);
      query.set('right', direct[1] + normalMax);
    }
    return query.toString();
  }

  function transmit(action, signal) {
    return fetch('/drive?' + driveQuery(action), {
      method: 'POST', headers: { 'X-Drive': '1' }, cache: 'no-store', signal
    });
  }

  function sendMotion(action) {
    if (motion?.readyState !== WebSocket.OPEN) return false;
    motion.send(driveQuery(action));
    outstanding++;
    return true;
  }

  function release(clear = false) {
    const sendStop = held || clear;
    pointer = null;
    keys.clear();
    held = false;
    direct = null;
    runKey = null;
    setStick(0, 0);
    stick.removeAttribute('data-held');
    run.removeAttribute('data-held');
    const action = clear ? 1 : 0;
    if (session && sendStop && !sendMotion(action)) transmit(action, AbortSignal.timeout(300)).catch(() => {});
    updateButtons();
  }

  function disconnectMotion(channel) {
    if (motion !== channel) return;
    motion = null;
    outstanding = 0;
    channel?.close();
    if (held) {
      driveNotice = 'Stopped · no reply from HotelBot';
      release();
    }
    session = 0;
    updateButtons();
    status.textContent = driveNotice || 'Waiting for HotelBot…';
  }

  function openMotion() {
    const channel = new WebSocket('ws://' + location.host + '/motion');
    motion = channel;
    const timeout = setTimeout(() => disconnectMotion(channel), 1800);
    channel.addEventListener('open', () => {
      clearTimeout(timeout);
      if (motion === channel) updateButtons();
    });
    channel.addEventListener('close', () => {
      clearTimeout(timeout);
      disconnectMotion(channel);
    });
    channel.addEventListener('message', event => {
      if (motion !== channel) return;
      outstanding = Math.max(0, outstanding - 1);
      if (!held) return;
      let state;
      try {
        state = JSON.parse(event.data);
      } catch {
        disconnectMotion(channel);
        return;
      }
      lastDriveReplyAt = performance.now();
      if (state.rejection === 'expired' || state.rejection === 'session') {
        release();
        disconnectMotion(channel);
        driveNotice = state.rejection === 'expired' ? 'Stopped · drive heartbeat expired'
          : 'Stopped · control session ended';
      } else if (state.rejection && state.rejection !== 'sequence') {
        driveNotice = 'Stopped · drive command rejected';
        release();
      }
      renderState(state);
    });
  }

  function pulse() {
    if (!session || !held) return;
    if (performance.now() - lastDriveReplyAt >= 450) {
      disconnectMotion(motion);
    } else if (outstanding < 3 && !motion?.bufferedAmount) {
      sendMotion(0);
    }
  }

  function setStick(dx, dy) {
    const magnitude = Math.hypot(dx, dy);
    const scale = magnitude > 1 ? 1 / magnitude : 1;
    const radius = (stick.clientWidth - knob.offsetWidth) / 2;
    knob.style.transform = 'translate(' + dx * scale * radius + 'px,' + dy * scale * radius + 'px)';
    const commandRadius = Math.max(0, (Math.min(magnitude, 1) - 0.12) / 0.88);
    x = magnitude ? Math.round(100 * dx / magnitude * commandRadius) : 0;
    y = magnitude ? Math.round(-100 * dy / magnitude * commandRadius) : 0;
  }

  function track(event) {
    const bounds = stick.getBoundingClientRect();
    const radius = (bounds.width - knob.offsetWidth) / 2;
    setStick((event.clientX - bounds.left - bounds.width / 2) / radius,
             (event.clientY - bounds.top - bounds.height / 2) / radius);
  }

  function begin() {
    driveNotice = '';
    lastDriveReplyAt = performance.now();
    held = true;
    (direct ? run : stick).setAttribute('data-held', '');
    status.textContent = 'Driving';
    updateButtons();
    pulse();
  }

  stick.addEventListener('pointerdown', event => {
    if (event.button !== 0 || held || stick.disabled) return;
    event.preventDefault();
    pointer = event.pointerId;
    stick.setPointerCapture(pointer);
    track(event);
    begin();
  });
  stick.addEventListener('pointermove', event => { if (pointer === event.pointerId) track(event); });
  for (const name of ['pointerup', 'pointercancel', 'lostpointercapture']) {
    stick.addEventListener(name, event => { if (pointer === event.pointerId) release(); });
  }
  stick.addEventListener('contextmenu', event => event.preventDefault());

  run.addEventListener('pointerdown', event => {
    if (event.button !== 0 || held || run.disabled) return;
    event.preventDefault();
    run.setPointerCapture(event.pointerId);
    pointer = event.pointerId;
    direct = directTargets();
    begin();
  });
  for (const name of ['pointerup', 'pointercancel', 'lostpointercapture']) {
    run.addEventListener(name, event => { if (direct && pointer === event.pointerId) release(); });
  }
  run.addEventListener('contextmenu', event => event.preventDefault());
  run.addEventListener('keydown', event => {
    if (event.key !== ' ' && event.key !== 'Enter') return;
    event.preventDefault();
    if (event.repeat || held || run.disabled) return;
    runKey = event.key;
    direct = directTargets();
    begin();
  });
  run.addEventListener('blur', () => { if (runKey) release(); });
  for (const input of setpoints) input.addEventListener('input', updateButtons);

  function keyboardStick() {
    setStick(Number(keys.has('ArrowRight')) - Number(keys.has('ArrowLeft')),
             Number(keys.has('ArrowDown')) - Number(keys.has('ArrowUp')));
  }
  document.addEventListener('keydown', event => {
    if (event.key === 'Escape') release();
    if (!arrowKeys.includes(event.key) || direct || stick.disabled || pointer !== null ||
        event.target.closest('input, select, textarea, button:not(#stick)')) return;
    event.preventDefault();
    if (event.repeat) return;
    keys.add(event.key);
    keyboardStick();
    if (!held) begin();
  });
  document.addEventListener('keyup', event => {
    if (runKey === event.key) { release(); return; }
    if (!keys.delete(event.key)) return;
    if (keys.size) keyboardStick();
    else release();
  });

  stop.addEventListener('click', () => {
    driveNotice = '';
    release(true);
  });
  overrideButton.addEventListener('click', async () => {
    if (!session || overridePending || held || overrideButton.hidden) return;
    overridePending = true;
    overrideButton.disabled = true;
    try {
      const response = await transmit(2, AbortSignal.timeout(500));
      if (!response.ok) throw Error();
    } catch {
      session = 0;
      updateButtons();
      status.textContent = 'Could not enable Hall override';
    } finally {
      overridePending = false;
      pollStatus();
    }
  });
  for (const button of graphButtons) {
    button.addEventListener('click', () => {
      graphWheel = button.dataset.wheel;
      for (const choice of graphButtons) choice.setAttribute('aria-pressed', choice === button);
      drawGraphs();
    });
  }
  pauseGraphs.addEventListener('click', () => {
    pausedHistory = pausedHistory ? null : history.slice();
    pauseGraphs.textContent = pausedHistory ? 'Resume graphs' : 'Pause graphs';
    drawGraphs();
  });
  window.addEventListener('resize', drawGraphs);
  for (const button of tuneButtons) {
    button.addEventListener('click', async () => {
      const side = button.dataset.tune;
      if (side === 'read') {
        readGains();
        tuneStatus.textContent = 'Loaded current gains.';
        return;
      }
      const query = new URLSearchParams({ session, side });
      if (side !== 's') {
        const inputs = gainInputs[side === 'l' ? 0 : 1];
        if (!inputs.every(input => input.reportValidity())) return;
        inputs.forEach(input => query.set(input.name, input.valueAsNumber));
      }
      const request = { side, id: 0 };
      tuningRequest = request;
      updateButtons();
      tuneStatus.textContent = side === 's' ? 'Saving…' : 'Applying…';
      try {
        const response = await fetch('/tune?' + query, {
          method: 'POST', headers: { 'X-Drive': '1' }, signal: AbortSignal.timeout(1000)
        });
        if (!response.ok) throw Error(response.status === 409 ? 'Stop the wheels and reconnect before tuning.' : 'Could not change tuning.');
        request.id = (await response.json()).id;
        pollStatus();
        setTimeout(() => {
          if (tuningRequest !== request) return;
          tuningRequest = null;
          tuneStatus.textContent = 'No confirmation. Read current gains to check.';
          updateButtons();
        }, 2500);
      } catch (error) {
        tuningRequest = null;
        tuneStatus.textContent = error.name === 'TimeoutError' ? 'Connection timed out.' : error.message;
        updateButtons();
      }
    });
  }
  limit.addEventListener('input', () => { limitValue.value = limit.value + ' RPM'; });
  window.addEventListener('blur', () => release());
  window.addEventListener('pagehide', () => release());
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) release();
    else pollStatus();
  });
  setInterval(pulse, 75);
  setInterval(pollStatus, 125);
  connect();
});
</script>
</head>
<body>
<main>
  <header>
    <h1>HotelBot<br>Drive</h1>
    <p id="status" role="status">Connecting…</p>
    <p class="guide">Drag to drive. Release to coast.</p>
    <dl class="wheel-speeds" aria-label="Estimated and target wheel RPM">
      <div><dt>Left RPM</dt><dd id="left-rpm">— / —</dd></div>
      <div><dt>Right RPM</dt><dd id="right-rpm">— / —</dd></div>
    </dl>
    <p class="reading-label">Estimated / target</p>
    <div class="direct">
      <h2>Direct wheel speed</h2>
      <p class="reading-label" id="direct-help">Positive is forward, negative is reverse. Use 0 to leave a wheel unpowered. Minimum moving speed is 3 RPM.</p>
      <div class="numbers">
        <label>Left RPM <input type="number" min="-240" max="240" step="1" value="0" required aria-describedby="direct-help"></label>
        <label>Right RPM <input type="number" min="-240" max="240" step="1" value="0" required aria-describedby="direct-help"></label>
      </div>
      <button id="run" type="button" disabled>Hold to run</button>
    </div>
    <noscript><p>Enable JavaScript to use the controller.</p></noscript>
  </header>
  <section class="controls" aria-label="Drive controls">
    <button id="stick" type="button" aria-label="Drive joystick. Drag or hold arrow keys." disabled>
      <span class="up" aria-hidden="true">↑</span><span class="down" aria-hidden="true">↓</span>
      <span class="left" aria-hidden="true">←</span><span class="right" aria-hidden="true">→</span>
      <span id="knob" aria-hidden="true"></span>
    </button>
    <div class="speed">
      <label for="limit">Speed limit</label>
      <output for="limit" id="limit-value">40 RPM</output>
      <input id="limit" type="range" min="5" max="240" value="40">
    </div>
    <button id="stop" type="button">Stop</button>
    <button id="override" type="button" hidden>Drive with Hall override</button>
  </section>
  <section class="panel" aria-labelledby="graphs-title">
    <div class="graph-head">
      <h2 id="graphs-title">Live wheel graphs</h2>
      <div class="graph-actions">
        <button id="pause-graphs" type="button" aria-controls="wheel-graphs">Pause graphs</button>
        <div class="wheel-pick" role="group" aria-label="Wheel to graph">
          <button type="button" data-wheel="left" aria-pressed="true">Left wheel</button>
          <button type="button" data-wheel="right" aria-pressed="false">Right wheel</button>
        </div>
      </div>
    </div>
    <div class="charts" id="wheel-graphs">
      <figure>
        <figcaption>Speed · RPM</figcaption>
        <div class="legend"><span style="--line:#355a91">Target</span><span style="--line:#226344">Estimated</span></div>
        <canvas role="img" aria-label="Live target and estimated wheel speed"></canvas>
      </figure>
      <figure>
        <figcaption>Error · RPM</figcaption>
        <div class="legend"><span style="--line:#8e531c">Target minus estimated</span></div>
        <canvas role="img" aria-label="Live speed tracking error"></canvas>
      </figure>
      <figure>
        <figcaption>PID terms · PWM percentage points</figcaption>
        <div class="legend"><span style="--line:#355a91">P</span><span style="--line:#226344">I</span><span style="--line:#98506f">D</span></div>
        <canvas role="img" aria-label="Live proportional, integral and derivative contributions"></canvas>
      </figure>
      <figure>
        <figcaption>Throttle · %</figcaption>
        <div class="legend"><span style="--line:#226344">Applied PWM</span></div>
        <canvas role="img" aria-label="Live applied throttle"></canvas>
      </figure>
    </div>
    <div class="tune">
      <h2>PID tuning</h2>
      <div class="gain-rows">
        <fieldset class="gains">
          <legend>Left wheel</legend>
          <div class="gain-inputs">
            <label>Kp <input name="kp" type="number" min="0" max="1" step="any" required></label>
            <label>Ki <input name="ki" type="number" min="0" max="2" step="any" required></label>
            <label>Kd <input name="kd" type="number" min="-1" max="1" step="any" required></label>
          </div>
          <button type="button" data-tune="l" disabled>Apply left gains</button>
        </fieldset>
        <fieldset class="gains">
          <legend>Right wheel</legend>
          <div class="gain-inputs">
            <label>Kp <input name="kp" type="number" min="0" max="1" step="any" required></label>
            <label>Ki <input name="ki" type="number" min="0" max="2" step="any" required></label>
            <label>Kd <input name="kd" type="number" min="-1" max="1" step="any" required></label>
          </div>
          <button type="button" data-tune="r" disabled>Apply right gains</button>
        </fieldset>
      </div>
      <div class="tune-actions">
        <button type="button" data-tune="read" disabled>Read gains</button>
        <button type="button" data-tune="s" disabled>Save applied gains</button>
      </div>
      <p id="tune-status" role="status"></p>
    </div>
  </section>
</main>
</body>
</html>
)HTML";
