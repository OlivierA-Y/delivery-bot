"use strict";

const byId = (id) => document.getElementById(id);
const stick = byId("stick");
const knob = byId("stick-knob");
const throttle = byId("throttle");
const clearance = byId("clear");
const connection = byId("connection");
const faultLabels = {
  left_stall: "Left wheel stalled",
  right_stall: "Right wheel stalled",
  left_hall: "Left Hall signal invalid",
  right_hall: "Right Hall signal invalid",
  left_direction: "Left wheel moved opposite to drive",
  right_direction: "Right wheel moved opposite to drive",
};
const keys = new Map([
  ["ArrowUp", "forward"], ["KeyW", "forward"],
  ["ArrowDown", "reverse"], ["KeyS", "reverse"],
  ["ArrowLeft", "left"], ["KeyA", "left"],
  ["ArrowRight", "right"], ["KeyD", "right"],
]);
const pressed = new Set();

let status;
let active;
let starting = false;
let stopsPending = 0;
let notice = "";
let axes = { x: 0, y: 0 };

function setText(element, value) {
  if (element.textContent !== value) element.textContent = value;
}

function canStart() {
  return status?.available && clearance.checked && !document.hidden
    && !active && !starting && stopsPending === 0;
}

function render() {
  const state = status?.state;
  const fault = state?.faults.length;
  const ready = status?.available;
  const limit = status?.max_drive_rpm ?? 40;
  stick.dataset.held = Boolean(active);
  stick.disabled = !active && !canStart();
  throttle.max = String(limit);
  setText(byId("throttle-value"), throttle.value + " RPM");
  setText(byId("throttle-max"), limit + " RPM max");
  connection.dataset.mode = !state ? "offline" : fault ? "fault" : state.armed ? "armed" : ready ? "ready" : "idle";
  setText(connection, !state ? "No telemetry" : fault ? "Drive fault" : state.armed ? "Drive active" : ready ? "Ready" : "Stopped");
  setText(byId("drive-left"), state ? rpm(state.left_deg_s) : "—");
  setText(byId("drive-right"), state ? rpm(state.right_deg_s) : "—");
  setText(byId("drive-left-target"), rpm(status?.target?.[0] ?? 0));
  setText(byId("drive-right-target"), rpm(status?.target?.[1] ?? 0));
  const message = fault ? state.faults.map((code) => faultLabels[code] ?? code).join(", ")
    : status?.last_drive_error || notice || status?.message || "Waiting for ESP32 telemetry.";
  setText(byId("drive-message"), message);
}

function rpm(degreesPerSecond) {
  return (degreesPerSecond / 6).toFixed(1);
}

function stickRadius() {
  return (stick.clientWidth - knob.offsetWidth) / 2;
}

function moveStick(x, y) {
  const magnitude = Math.hypot(x, y);
  const scale = Math.max(1, magnitude);
  const radius = stickRadius();
  stick.style.setProperty("--stick-x", x / scale * radius + "px");
  stick.style.setProperty("--stick-y", -y / scale * radius + "px");
  axes = magnitude < .08 ? { x: 0, y: 0 } : { x: x / scale, y: y / scale };
}

function trackPointer(event) {
  const bounds = stick.getBoundingClientRect();
  const radius = stickRadius();
  moveStick((event.clientX - bounds.left - bounds.width / 2) / radius,
    (bounds.top + bounds.height / 2 - event.clientY) / radius);
}

function trackKeys() {
  const directions = new Set([...pressed].map((key) => keys.get(key)));
  moveStick(Number(directions.has("right")) - Number(directions.has("left")),
    Number(directions.has("forward")) - Number(directions.has("reverse")));
}

async function post(action, body) {
  const response = await fetch("/api/" + action, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
    keepalive: true,
    signal: AbortSignal.timeout(650),
  });
  const data = await response.json();
  if (!response.ok) throw Object.assign(new Error(data.error || "Request failed."), { status: response.status });
  return data;
}

async function stop(run) {
  stopsPending += 1;
  render();
  try {
    await post("stop", run ? { run } : {});
  } catch {
    notice = "Stop request did not reach the Pi. Drive expires without a heartbeat.";
  } finally {
    stopsPending -= 1;
    render();
  }
}

function release(reason = "", force = false) {
  const run = active;
  if (!run && !force) return;
  clearTimeout(run?.timer);
  active = undefined;
  pressed.clear();
  if (run?.pointer != null && stick.hasPointerCapture(run.pointer)) stick.releasePointerCapture(run.pointer);
  moveStick(0, 0);
  notice = reason;
  if (force) {
    clearance.checked = false;
    stop();
  } else if (run?.key) {
    stop(run.key);
  }
  render();
}

async function renew(run) {
  if (active !== run) return;
  try {
    await post("hold", { run: run.key, ...axes, rpm: throttle.valueAsNumber });
    if (active === run) run.timer = setTimeout(() => renew(run), 50);
  } catch (error) {
    if (active === run) handleDriveError(error);
  }
}

async function begin(pointer = null) {
  if (!canStart()) return;
  const epoch = status.epoch;
  const run = { pointer };
  active = run;
  starting = true;
  status.available = false;
  notice = "";
  render();
  try {
    const reservation = await post("drive/start", { epoch, clear: true });
    run.key = reservation.run;
    if (active === run) renew(run);
    else await stop(run.key);
  } catch (error) {
    if (active === run) handleDriveError(error);
  } finally {
    starting = false;
    render();
  }
}

function clearDrive(reason = "") {
  clearance.checked = false;
  release(reason);
  render();
}

function handleDriveError(error) {
  if (error.status === 409) release(error.message);
  else clearDrive(error.message);
}

async function refresh() {
  try {
    const response = await fetch("/api/status", { cache: "no-store", signal: AbortSignal.timeout(750) });
    if (!response.ok) throw new Error("Status unavailable.");
    const previousBoot = status?.boot;
    status = await response.json();
    if (previousBoot && status.boot !== previousBoot) {
      clearDrive(status.message || "Drive feedback changed. Check the controller status.");
    } else if (!status.state) {
      release(status.last_drive_error || status.message);
    } else if (status.state.faults.length) {
      release(status.state.faults.map((code) => faultLabels[code] ?? code).join(", "));
    }
  } catch {
    status = undefined;
    clearDrive("The Pi stopped responding.");
  }
  render();
  setTimeout(refresh, 125);
}

clearance.checked = false;
clearance.addEventListener("change", () => {
  if (!clearance.checked) release();
  else notice = "";
  render();
});

throttle.addEventListener("input", render);
stick.addEventListener("pointerdown", (event) => {
  if (event.button !== 0 || !event.isPrimary || !canStart()) return;
  event.preventDefault();
  stick.setPointerCapture(event.pointerId);
  trackPointer(event);
  begin(event.pointerId);
});
stick.addEventListener("pointermove", (event) => {
  if (active?.pointer === event.pointerId) trackPointer(event);
});
for (const type of ["pointerup", "pointercancel", "lostpointercapture"]) {
  stick.addEventListener(type, (event) => {
    if (active?.pointer === event.pointerId) release();
  });
}
stick.addEventListener("contextmenu", (event) => event.preventDefault());

window.addEventListener("keydown", (event) => {
  if (event.code === "Escape") {
    if (!event.repeat) release("", true);
    return;
  }
  if (event.ctrlKey || event.metaKey || event.altKey || event.isComposing) return;
  if (!keys.has(event.code) || active?.pointer != null
      || (event.target === throttle && event.code.startsWith("Arrow"))) return;
  if (!active && !canStart()) return;
  event.preventDefault();
  if (event.repeat) return;
  pressed.add(event.code);
  trackKeys();
  if (!active) begin();
});
window.addEventListener("keyup", (event) => {
  if (!pressed.delete(event.code)) return;
  if (pressed.size) trackKeys();
  else release();
});
byId("drive-stop").addEventListener("click", () => release("", true));
window.addEventListener("blur", () => clearDrive());
window.addEventListener("pagehide", () => clearDrive());
document.addEventListener("visibilitychange", () => { if (document.hidden) clearDrive(); });

refresh();
