"use strict";

const byId = (id) => document.getElementById(id);
const wheels = ["left", "right"];
const inputs = wheels.map((wheel) => byId(`${wheel}-target`));
const presets = [...document.querySelectorAll("[data-targets]")];
const lifted = byId("lifted");
const hold = byId("hold");
const samples = [];
const historyMs = 12000;
let status;
let lastBoot;
let active = null;
let notice = "";
let stopping = false;

function setText(element, text) {
  if (element.textContent !== text) element.textContent = text;
}

function targets() {
  const values = inputs.map((input) => input.valueAsNumber);
  return values.some((value) => value !== 0) && inputs.every((input) => input.validity.valid)
    && values.every(Number.isFinite) ? values : null;
}

async function post(action, data) {
  const response = await fetch(`/api/${action}`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(data),
    keepalive: true,
    signal: AbortSignal.timeout(650),
  });
  const result = await response.json();
  if (!response.ok) throw new Error(result.error);
  return result;
}

function release(force = false, reason = "") {
  if (!active && !force) return;
  if (active) clearTimeout(active.timer);
  active = null;
  notice = reason;
  stopping = true;
  render();
  post("stop", {}).catch(() => {
    notice = "Stop request could not reach the Pi. The controller watchdog applies.";
  }).finally(() => {
    stopping = false;
    render();
  });
}

async function renew(run) {
  if (active !== run) return;
  try {
    await post("hold", { run: run.key });
    if (active === run) run.timer = setTimeout(() => renew(run), 80);
  } catch (error) {
    if (active === run) release(false, error.message);
  }
}

async function begin() {
  const values = targets();
  if (active || hold.disabled || !status?.available || !lifted.checked || !values || document.hidden) return;
  const run = {};
  active = run;
  notice = "";
  render();
  try {
    const result = await post("start", {
      epoch: status.epoch, lifted: true, left: values[0], right: values[1],
    });
    if (active === run) {
      run.key = result.run;
      renew(run);
    }
  } catch (error) {
    if (active === run) release(false, error.message);
  }
}

function signed(value, decimals) {
  if (value === undefined) return "—";
  return (value > 0 ? "+" : "") + value.toFixed(decimals);
}

function render() {
  const state = status?.state;
  hold.disabled = !active && (!status?.available || !lifted.checked || !targets() || stopping);
  hold.dataset.held = Boolean(active);
  setText(hold, active ? "Holding" : "Hold to run");
  inputs.forEach((input) => { input.disabled = Boolean(active); });
  presets.forEach((preset) => { preset.disabled = Boolean(active); });
  const fault = Boolean(state?.faults.length);
  const board = byId("board-status");
  board.dataset.mode = !state ? "offline" : fault ? "fault" : state.armed ? "armed" : status.available ? "ready" : "idle";
  setText(board, !state ? "No telemetry" : fault ? "Drive fault" : state.armed ? "Armed" : status.available ? "Ready · disarmed" : "Disarmed");
  setText(byId("boot"), status?.boot ? `ESP32 · ${status.boot.slice(0, 8)}` : "ESP32");
  const elapsed = status?.elapsed ?? 0;
  const limit = status?.limit ?? 4;
  byId("elapsed").max = limit;
  byId("elapsed").value = elapsed;
  setText(byId("timer-label"), `${elapsed.toFixed(1)} / ${limit.toFixed(1)} s`);
  const message = fault ? `Faults · ${state.faults.join(", ")}` : notice || status?.message || "Waiting for ESP32 telemetry.";
  setText(byId("message"), message);
  for (const wheel of wheels) {
    setText(byId(`${wheel}-speed`), signed(state?.[`${wheel}_deg_s`], 1));
    setText(byId(`${wheel}-count`), signed(state?.[`${wheel}_count`], 0));
  }
}

function drawChart(canvas, index, now) {
  const width = canvas.clientWidth;
  const height = canvas.clientHeight;
  const scale = Math.min(devicePixelRatio || 1, 2);
  canvas.width = Math.round(width * scale);
  canvas.height = Math.round(height * scale);
  const context = canvas.getContext("2d");
  context.scale(scale, scale);
  const left = 35, top = 11, bottom = height - 12;
  const limit = 120;
  const x = (stamp) => width - 4 - (now - stamp) * (width - left - 4) / historyMs;
  const y = (value) => top + (limit - Math.min(limit, Math.max(-limit, value))) * (bottom - top) / (limit * 2);
  context.font = "12px ui-monospace, monospace";
  context.textBaseline = "middle";
  context.textAlign = "right";
  for (const level of [limit, 0, -limit]) {
    const row = y(level);
    context.fillStyle = "#adbcb9";
    context.fillText(level, left - 7, row);
    context.strokeStyle = level === 0 ? "#526263" : "#354144";
    context.beginPath();
    context.moveTo(left, row);
    context.lineTo(width - 4, row);
    context.stroke();
  }
  context.save();
  context.beginPath();
  context.rect(left, 0, width - left, height);
  context.clip();
  for (const [key, color, dash] of [["target", "#ffbc76", [5, 4]], ["actual", "#8bdfc0", []]]) {
    context.strokeStyle = color;
    context.lineWidth = 1.8;
    context.setLineDash(dash);
    context.beginPath();
    let connected = false;
    for (const sample of samples) {
      const value = sample[key]?.[index];
      if (value === undefined) {
        connected = false;
        continue;
      }
      const point = [x(sample.stamp), y(value)];
      if (connected) context.lineTo(...point);
      else context.moveTo(...point);
      connected = true;
    }
    context.stroke();
  }
  context.restore();
}

function charts() {
  const now = performance.now();
  while (samples.length && samples[0].stamp < now - historyMs - 250) samples.shift();
  wheels.forEach((wheel, index) => drawChart(byId(`${wheel}-chart`), index, now));
}

async function refresh() {
  try {
    const response = await fetch("/api/status", { cache: "no-store", signal: AbortSignal.timeout(750) });
    if (!response.ok) throw new Error("Status unavailable.");
    status = await response.json();
    if (lastBoot && status.boot !== lastBoot) samples.length = 0;
    lastBoot = status.boot;
    const state = status.state;
    samples.push({
      stamp: performance.now(), actual: state ? [state.left_deg_s, state.right_deg_s] : null,
      target: state ? status.target : null,
    });
    if (!state && active) release(false, "ESP32 telemetry was lost.");
  } catch {
    status = undefined;
    samples.push({ stamp: performance.now() });
    if (active) release(false, "The Pi stopped responding.");
  }
  render();
  charts();
  setTimeout(refresh, 130);
}

for (const preset of presets) {
  preset.addEventListener("click", () => {
    preset.dataset.targets.split(",").forEach((value, index) => { inputs[index].value = value; });
    render();
  });
}
inputs.forEach((input) => input.addEventListener("input", render));
lifted.addEventListener("change", () => {
  if (!lifted.checked) release();
  render();
});

hold.addEventListener("pointerdown", (event) => {
  if (event.button !== 0 || hold.disabled) return;
  event.preventDefault();
  hold.setPointerCapture(event.pointerId);
  begin();
});
window.addEventListener("pointerup", () => release());
hold.addEventListener("pointercancel", () => release());
hold.addEventListener("lostpointercapture", () => release());
hold.addEventListener("keydown", (event) => {
  if (event.key !== " " && event.key !== "Enter") return;
  event.preventDefault();
  if (!event.repeat) begin();
});
window.addEventListener("keyup", (event) => {
  if (event.key === " " || event.key === "Enter") release();
});
window.addEventListener("keydown", (event) => {
  if (event.key === "Escape") release(true);
});
byId("stop").addEventListener("click", () => release(true));
window.addEventListener("blur", () => release());
window.addEventListener("pagehide", () => release());
document.addEventListener("visibilitychange", () => { if (document.hidden) release(); });
window.addEventListener("resize", charts);

refresh();
