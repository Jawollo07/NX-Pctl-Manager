(() => {
"use strict";
const $ = (id) => document.getElementById(id);
let adminToken = "";
let devices = [];
let selectedDevice = null;
let selectedPolicy = null;
const weekdays = ["Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"];
const state = { toastTimer: null };

function toast(message, error = false) {
  const el = $("toast");
  el.textContent = message;
  el.style.borderColor = error ? "#a64e5b" : "#5275a5";
  el.classList.add("show");
  clearTimeout(state.toastTimer);
  state.toastTimer = setTimeout(() => el.classList.remove("show"), 3600);
}
function dateTime(value) {
  if (!value) return "Noch nie";
  const date = new Date(Number(value) * 1000);
  return Number.isNaN(date.getTime()) ? "—" : date.toLocaleString("de-DE");
}
function escapeText(value) { return String(value ?? ""); }
async function api(path, options = {}) {
  const headers = new Headers(options.headers || {});
  headers.set("Authorization", "Bearer " + adminToken);
  if (options.body !== undefined) headers.set("Content-Type", "application/json");
  const response = await fetch(path, {...options, headers, cache: "no-store", credentials: "same-origin"});
  const raw = await response.text();
  let data = {};
  try { data = raw ? JSON.parse(raw) : {}; } catch { data = {detail: raw || "Ungültige Serverantwort"}; }
  if (!response.ok) {
    const detail = typeof data.detail === "string" ? data.detail : JSON.stringify(data.detail || data);
    if (response.status === 401) logout();
    throw new Error(detail || ("HTTP " + response.status));
  }
  return data;
}
function setConnected(connected) {
  $("connection").textContent = connected ? "Backend verbunden" : "Nicht verbunden";
  $("connection").classList.toggle("online", connected);
  $("refresh").disabled = !connected;
  $("logout").hidden = !connected;
}
function logout() {
  adminToken = "";
  devices = [];
  selectedDevice = null;
  selectedPolicy = null;
  $("admin-token").value = "";
  $("app-view").hidden = true;
  $("login-view").hidden = false;
  $("login-error").textContent = "";
  setConnected(false);
}
async function connect() {
  adminToken = $("admin-token").value.trim();
  try {
    await loadDevices();
    $("login-view").hidden = true;
    $("app-view").hidden = false;
    setConnected(true);
  } catch (error) {
    $("login-error").textContent = error.message;
    adminToken = "";
  }
}
function makeCell(text, className) {
  const td = document.createElement("td");
  td.textContent = text;
  if (className) td.className = className;
  return td;
}
function renderDevices() {
  const body = $("devices-body");
  body.replaceChildren();
  $("device-count").textContent = String(devices.length);
  $("stat-total").textContent = String(devices.length);
  $("stat-active").textContent = String(devices.filter(d => !d.revoked_at).length);
  $("stat-parent").textContent = String(devices.filter(d => d.parent_auth_configured).length);
  const latest = devices.filter(d => d.last_seen_at).sort((a,b) => b.last_seen_at-a.last_seen_at)[0];
  $("stat-online").textContent = latest ? dateTime(latest.last_seen_at) : "—";
  if (!devices.length) {
    body.append(makeCell("Keine Geräte registriert.", "empty"));
    body.firstChild.colSpan = 4;
    return;
  }
  for (const device of devices) {
    const tr = document.createElement("tr");
    tr.className = "device-row" + (selectedDevice === device.device_id ? " selected" : "");
    tr.tabIndex = 0;
    tr.setAttribute("role", "button");
    tr.addEventListener("click", () => selectDevice(device.device_id));
    tr.addEventListener("keydown", e => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); selectDevice(device.device_id); } });
    const name = document.createElement("td");
    const title = document.createElement("div"); title.className = "device-name"; title.textContent = device.device_id;
    const sub = document.createElement("div"); sub.className = "device-sub"; sub.textContent = device.parent_auth_configured ? "Elternauth eingerichtet" : "Keine Elternauth";
    name.append(title, sub); tr.append(name);
    const status = makeCell(device.revoked_at ? "Widerrufen" : "Registriert");
    status.className = device.revoked_at ? "bad" : "good"; tr.append(status);
    tr.append(makeCell(device.policy_revision ? "v" + device.policy_revision : "Nicht gesetzt"));
    tr.append(makeCell(dateTime(device.last_seen_at)));
    body.append(tr);
  }
}
async function loadDevices() {
  const result = await api("/v1/admin/devices");
  devices = Array.isArray(result.devices) ? result.devices : [];
  renderDevices();
}
async function selectDevice(id) {
  selectedDevice = id;
  selectedPolicy = null;
  renderDevices();
  $("empty-selection").hidden = true;
  $("device-detail").hidden = false;
  $("selected-name").textContent = id;
  const device = devices.find(d => d.device_id === id);
  $("selected-meta").textContent = "Erstellt: " + dateTime(device?.created_at) + " · Zuletzt gesehen: " + dateTime(device?.last_seen_at);
  $("selected-status").textContent = device?.revoked_at ? "Widerrufen" : "Registriert";
  $("selected-status").className = "pill " + (device?.revoked_at ? "bad" : "good");
  await loadPolicy();
  await loadParentStatus();
}
function timeToMinutes(value) {
  const [h,m] = String(value || "00:00").split(":").map(Number);
  return h * 60 + m;
}
function minutesToTime(value) {
  const n = Math.max(0, Math.min(1439, Number(value) || 0));
  return String(Math.floor(n/60)).padStart(2,"0") + ":" + String(n%60).padStart(2,"0");
}
function buildWeekdays(values = [65535,65535,65535,65535,65535,65535,65535]) {
  const host = $("weekday-fields"); host.replaceChildren();
  weekdays.forEach((day, index) => {
    const label = document.createElement("label"); label.textContent = day;
    const input = document.createElement("input");
    input.type = "number"; input.min = "0"; input.max = "65535"; input.required = true;
    input.dataset.day = String(index); input.value = String(values[index] ?? 65535);
    label.append(input); host.append(label);
  });
}
function fillPolicy(policy) {
  $("policy-enabled").checked = !!policy.enabled;
  $("policy-parent").checked = !!policy.require_parent_authentication;
  $("daily-limit").value = policy.global_daily_limit_minutes ?? 90;
  $("timezone").value = policy.timezone_offset_minutes ?? 60;
  $("offline").value = String(policy.offline_behavior ?? 0);
  $("quiet-enabled").checked = !!policy.quiet_hours_enabled;
  $("quiet-start").value = minutesToTime(policy.quiet_hours_start_minute ?? 1320);
  $("quiet-end").value = minutesToTime(policy.quiet_hours_end_minute ?? 420);
  buildWeekdays(policy.weekday_limit_minutes);
  $("policy-revision").textContent = policy.revision ? "Aktuelle Revision: " + policy.revision : "Noch keine Richtlinie veröffentlicht";
}
async function loadPolicy() {
  try {
    const result = await api("/v1/admin/devices/" + encodeURIComponent(selectedDevice) + "/policy");
    selectedPolicy = result.policy;
    fillPolicy(selectedPolicy);
  } catch (error) {
    if (!error.message.includes("no policy configured")) throw error;
    selectedPolicy = null;
    fillPolicy({enabled:true,require_parent_authentication:false,global_daily_limit_minutes:90,weekday_limit_minutes:Array(7).fill(65535),timezone_offset_minutes:60,offline_behavior:0,quiet_hours_enabled:false,quiet_hours_start_minute:1320,quiet_hours_end_minute:420});
  }
}
async function publishPolicy(event) {
  event.preventDefault();
  if (!selectedDevice) return;
  const revision = Number(selectedPolicy?.revision || 0) + 1;
  const now = Math.floor(Date.now()/1000);
  const policy = {
    protocol_version:1, schema_version:1, device_id:selectedDevice, revision,
    issued_at:now, not_after:now + 7*24*60*60, server_time:now,
    enabled:$("policy-enabled").checked,
    require_parent_authentication:$("policy-parent").checked,
    offline_behavior:Number($("offline").value),
    timezone_offset_minutes:Number($("timezone").value),
    global_daily_limit_minutes:Number($("daily-limit").value),
    weekday_limit_minutes:Array.from($("weekday-fields").querySelectorAll("input")).map(x => Number(x.value)),
    quiet_hours_enabled:$("quiet-enabled").checked,
    quiet_hours_start_minute:timeToMinutes($("quiet-start").value),
    quiet_hours_end_minute:timeToMinutes($("quiet-end").value)
  };
  const result = await api("/v1/admin/devices/" + encodeURIComponent(selectedDevice) + "/policy", {method:"PUT",body:JSON.stringify(policy)});
  selectedPolicy = policy;
  $("policy-revision").textContent = "Veröffentlicht: Revision " + result.revision;
  toast("Richtlinie v" + result.revision + " veröffentlicht.");
  await loadDevices();
}
async function loadParentStatus() {
  const result = await api("/v1/admin/devices/" + encodeURIComponent(selectedDevice) + "/parent-auth");
  $("parent-status").textContent = !result.configured ? "Nicht eingerichtet. Richte TOTP ein, bevor du die Elternauthentifizierung in der Richtlinie verpflichtend aktivierst." :
    "Status: " + (result.authenticated ? "Freigabe aktiv" : "Keine aktive Freigabe") +
    " · Freigabe bis: " + (result.authenticated_until ? dateTime(result.authenticated_until) : "—") +
    " · Fehlversuche: " + (result.failed_attempts ?? 0) +
    " · Sperre bis: " + (result.locked_until ? dateTime(result.locked_until) : "—");
}
function showTab(name) {
  document.querySelectorAll(".tab").forEach(tab => tab.classList.toggle("active", tab.dataset.tab === name));
  ["policy","parent","events"].forEach(key => $("tab-" + key).hidden = key !== name);
  if (name === "parent") loadParentStatus().catch(e => toast(e.message,true));
  if (name === "events") loadEvents().catch(e => toast(e.message,true));
}
async function provisionParent() {
  if (!confirm("TOTP für " + selectedDevice + " neu einrichten/rotieren? Ein bisheriges Geheimnis und eine aktive Freigabe werden ungültig.")) return;
  const result = await api("/v1/admin/devices/" + encodeURIComponent(selectedDevice) + "/parent-auth", {method:"POST",body:JSON.stringify({confirm:true})});
  const box = $("totp-secret"); box.hidden = false; box.replaceChildren();
  const heading = document.createElement("strong"); heading.textContent = "TOTP-Geheimnis – nur jetzt sichtbar";
  const secret = document.createElement("div"); secret.className = "secret-value"; secret.textContent = result.totp_secret;
  const note = document.createElement("p"); note.className = "muted"; note.textContent = "In einer Authenticator-App einrichten. Dieses Geheimnis wird nicht erneut angezeigt. Bei Verlust muss es rotiert werden.";
  const copy = document.createElement("button"); copy.className = "button secondary"; copy.type = "button"; copy.textContent = "Geheimnis kopieren";
  copy.addEventListener("click", async () => { await navigator.clipboard.writeText(result.totp_secret); toast("Geheimnis kopiert."); });
  box.append(heading,secret,note,copy);
  toast("TOTP eingerichtet. Geheimnis jetzt sicher übertragen.");
  await loadParentStatus(); await loadDevices();
}
async function verifyParent(event) {
  event.preventDefault();
  const code = $("totp-code").value.trim();
  const result = await api("/v1/admin/devices/" + encodeURIComponent(selectedDevice) + "/parent-auth/verify", {method:"POST",body:JSON.stringify({code})});
  $("totp-code").value = "";
  toast("Elternauth freigegeben bis " + dateTime(result.authenticated_until));
  await loadParentStatus(); await loadDevices();
}
async function revokeParent() {
  if (!confirm("Elternauth-Konfiguration und aktive Freigabe wirklich entfernen?")) return;
  await api("/v1/admin/devices/" + encodeURIComponent(selectedDevice) + "/parent-auth", {method:"DELETE"});
  $("totp-secret").hidden = true; $("totp-secret").replaceChildren();
  toast("Elternauth entfernt.");
  await loadParentStatus(); await loadDevices();
}
async function loadEvents() {
  const result = await api("/v1/admin/devices/" + encodeURIComponent(selectedDevice) + "/events?limit=100");
  const host = $("events-list"); host.replaceChildren();
  if (!result.events.length) { const p=document.createElement("p"); p.className="muted"; p.textContent="Keine Ereignisse vorhanden."; host.append(p); return; }
  for (const item of result.events) {
    const article=document.createElement("article"); article.className="event";
    const title=document.createElement("strong"); title.textContent=item.type + " · #" + item.sequence;
    const time=document.createElement("time"); time.textContent=dateTime(item.received_at);
    const pre=document.createElement("pre"); pre.textContent=JSON.stringify(item.event?.payload ?? item.event,null,2);
    article.append(title,time,pre); host.append(article);
  }
}
async function createEnrollment() {
  const id = $("enroll-device-id").value.trim();
  const ttl = Number($("enroll-ttl").value);
  const result = await api("/v1/admin/devices/" + encodeURIComponent(id) + "/enrollment", {method:"POST",body:JSON.stringify({ttl_seconds:ttl})});
  const host=$("enrollment-result"); host.hidden=false; host.replaceChildren();
  const p=document.createElement("p"); p.className="muted"; p.textContent="Einmaliger Code für " + result.device_id + " · gültig bis " + dateTime(result.expires_at);
  const code=document.createElement("div"); code.className="dialog-code"; code.textContent=result.enrollment_code;
  const copy=document.createElement("button"); copy.type="button"; copy.className="button secondary"; copy.textContent="Code kopieren";
  copy.addEventListener("click",async()=>{await navigator.clipboard.writeText(result.enrollment_code);toast("Code kopiert.");});
  host.append(p,code,copy);
  $("create-enrollment").disabled=true;
  await loadDevices();
}
async function revokeDevice() {
  if (!confirm("Gerät " + selectedDevice + " wirklich widerrufen? Die Switch verliert den Backend-Zugriff.")) return;
  await api("/v1/admin/devices/" + encodeURIComponent(selectedDevice) + "/revoke", {method:"POST"});
  toast("Gerät widerrufen.");
  await loadDevices();
  $("selected-status").textContent="Widerrufen"; $("selected-status").className="pill bad";
}
function init() {
  weekdays.forEach(() => {});
  buildWeekdays();
  $("login-form").addEventListener("submit", e => {e.preventDefault();connect().catch(err => $("login-error").textContent=err.message);});
  $("refresh").addEventListener("click",()=>loadDevices().then(()=>selectedDevice&&selectDevice(selectedDevice)).catch(e=>toast(e.message,true)));
  $("logout").addEventListener("click",logout);
  $("new-enrollment").addEventListener("click",()=>{$("enrollment-result").hidden=true;$("enrollment-result").replaceChildren();$("create-enrollment").disabled=false;$("enrollment-dialog").showModal();});
  $("create-enrollment").addEventListener("click",()=>createEnrollment().catch(e=>toast(e.message,true)));
  $("policy-form").addEventListener("submit",e=>publishPolicy(e).catch(err=>toast(err.message,true)));
  $("load-policy").addEventListener("click",()=>loadPolicy().catch(e=>toast(e.message,true)));
  $("provision-parent").addEventListener("click",()=>provisionParent().catch(e=>toast(e.message,true)));
  $("verify-parent").addEventListener("submit",e=>verifyParent(e).catch(err=>toast(err.message,true)));
  $("revoke-parent").addEventListener("click",()=>revokeParent().catch(e=>toast(e.message,true)));
  $("revoke-device").addEventListener("click",()=>revokeDevice().catch(e=>toast(e.message,true)));
  $("load-events").addEventListener("click",()=>loadEvents().catch(e=>toast(e.message,true)));
  document.querySelectorAll(".tab").forEach(tab=>tab.addEventListener("click",()=>showTab(tab.dataset.tab)));
}
document.addEventListener("DOMContentLoaded",init);
})();