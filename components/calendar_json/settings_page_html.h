#pragma once

// The device settings page served at http://<device>/ by SettingsPage.
// Self-contained (no external requests): the panel's LAN may have no internet.
// The page is generic: it renders whatever fields /settings/state describes,
// grouped by their "group", and fills entity pickers from /settings/entities.

namespace esp32_dash {

static const char SETTINGS_PAGE_HTML[] = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Dashboard settings</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--text:#1d2026;--muted:#667085;--line:#e3e6ea;--accent:#2f6fde;--ok:#1a7f45;--warn:#b25e09;--err:#c0322b;--field:#fff}
@media (prefers-color-scheme:dark){:root{--bg:#121417;--card:#1c1f24;--text:#e7e9ec;--muted:#9aa3ae;--line:#2c3138;--accent:#6ca5ff;--ok:#4cc27c;--warn:#f0a447;--err:#ff6b62;--field:#15181c}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
header{position:sticky;top:0;z-index:2;background:var(--bg);border-bottom:1px solid var(--line)}
.wrap{max-width:760px;margin:0 auto;padding:0 16px}
header .wrap{display:flex;align-items:center;gap:12px;padding-top:14px;padding-bottom:12px;flex-wrap:wrap}
h1{font-size:19px;margin:0;flex:1;min-width:0}
#ha{font-size:13px;color:var(--muted);display:flex;align-items:center;gap:8px}
button{font:inherit;font-size:13px;padding:5px 11px;border-radius:7px;border:1px solid var(--line);background:var(--card);color:var(--text);cursor:pointer}
button:disabled{opacity:.5;cursor:default}
main{padding:16px 0 48px}
section{background:var(--card);border:1px solid var(--line);border-radius:12px;margin-bottom:16px;overflow:hidden}
h2{font-size:13px;letter-spacing:.06em;text-transform:uppercase;color:var(--muted);margin:0;padding:12px 16px 4px}
.row{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1.35fr);gap:4px 16px;align-items:start;padding:10px 16px;border-top:1px solid var(--line)}
section .row:first-of-type{border-top:0}
.row label{padding-top:6px;font-weight:500}
.ctl{display:flex;flex-direction:column;gap:3px;min-width:0}
.line{display:flex;align-items:center;gap:8px;min-width:0}
input[type=text],input[type=number],select{font:inherit;width:100%;min-width:0;padding:6px 9px;border:1px solid var(--line);border-radius:7px;background:var(--field);color:var(--text)}
input[type=number]{width:110px}
input:focus,select:focus{outline:2px solid var(--accent);outline-offset:-1px}
.unit{color:var(--muted)}
.hint{font-size:12.5px;color:var(--muted);overflow-wrap:anywhere}
.hint.warn{color:var(--warn)}
.st{font-size:12px;min-width:3.5em;color:var(--muted);white-space:nowrap}
.st.ok{color:var(--ok)}.st.err{color:var(--err)}
.switch{position:relative;width:42px;height:24px;flex:none;margin-top:3px}
.switch input{opacity:0;width:0;height:0}
.switch span{position:absolute;inset:0;border-radius:12px;background:var(--line);transition:.15s;cursor:pointer}
.switch span:before{content:"";position:absolute;width:18px;height:18px;left:3px;top:3px;border-radius:50%;background:#fff;transition:.15s}
.switch input:checked+span{background:var(--accent)}
.switch input:checked+span:before{transform:translateX(18px)}
.switch input:focus-visible+span{outline:2px solid var(--accent);outline-offset:2px}
#msg{padding:12px 16px;border-radius:10px;margin:16px 0 0;background:var(--card);border:1px solid var(--line)}
#msg.err{color:var(--err)}
@media (max-width:560px){.row{grid-template-columns:1fr}.row label{padding-top:0}}
</style>
</head>
<body>
<header><div class="wrap"><h1 id="title">Dashboard settings</h1>
<div id="ha"><span id="hastatus">Home Assistant entities: …</span><button id="refresh" type="button">Refresh</button></div></div></header>
<div class="wrap"><div id="msg" hidden></div><main id="main"></main></div>
<script>
"use strict";
const $ = (s) => document.querySelector(s);
const el = (tag, props, kids) => {
  const e = document.createElement(tag);
  if (props) for (const k in props) { if (k === "text") e.textContent = props[k]; else if (k in e) e[k] = props[k]; else e.setAttribute(k, props[k]); }
  (kids || []).forEach((c) => c && e.appendChild(c));
  return e;
};
let entities = [];        // [[id, friendly_name, unit], ...]
const byId = new Map();
const pickers = [];       // {input, hint, domains}
const lists = new Map();  // domains string -> <datalist>
const controls = new Map(); // key -> {set(value)}

function showMsg(text, isErr) {
  const m = $("#msg");
  m.hidden = !text; m.textContent = text || ""; m.className = isErr ? "err" : "";
}

async function post(key, value) {
  const body = "key=" + encodeURIComponent(key) + "&value=" + encodeURIComponent(value);
  const r = await fetch("/settings/set", { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" }, body });
  let j = {};
  try { j = await r.json(); } catch (e) {}
  if (!r.ok || !j.ok) throw new Error(j.error || ("HTTP " + r.status));
}

function status(st, text, cls) {
  st.textContent = text; st.className = "st" + (cls ? " " + cls : "");
  if (cls === "ok") setTimeout(() => { if (st.textContent === text) { st.textContent = ""; st.className = "st"; } }, 2000);
}

async function save(key, value, st, revert) {
  status(st, "Saving…");
  try {
    await post(key, value);
    status(st, "Saved", "ok");
    // The device applies writes on its main loop; read the result back.
    setTimeout(refreshValues, 400);
  } catch (e) {
    status(st, e.message, "err");
    if (revert) revert();
  }
}

function domainsOf(f) { return (f.domains || "").split(",").map((s) => s.trim()).filter(Boolean); }
function matches(id, domains) { return !domains.length || domains.includes(id.split(".")[0]); }

function datalistFor(domainsKey) {
  if (!lists.has(domainsKey)) {
    const dl = el("datalist", { id: "dl" + lists.size });
    document.body.appendChild(dl);
    lists.set(domainsKey, dl);
  }
  return lists.get(domainsKey);
}

function fillLists() {
  for (const [key, dl] of lists) {
    const domains = key ? key.split(",") : [];
    dl.textContent = "";
    for (const [id, name] of entities) if (matches(id, domains)) dl.appendChild(el("option", { value: id, label: name || id }));
  }
  pickers.forEach(updateHint);
}

function updateHint(p) {
  const v = p.input.value.trim();
  const h = p.hint;
  h.className = "hint";
  if (!v) { h.textContent = p.domains.length ? "Not set" : "Not set (tile hidden)"; return; }
  if (!/^[a-z0-9_]+\.[a-z0-9_]+$/.test(v)) { h.textContent = "Not an entity id (expected domain.object_id)"; h.className = "hint warn"; return; }
  if (!matches(v, p.domains)) { h.textContent = "Expected a " + p.domains.join(" / ") + " entity"; h.className = "hint warn"; return; }
  if (!entities.length) { h.textContent = ""; return; }
  const e = byId.get(v);
  if (!e) { h.textContent = "Not found in Home Assistant"; h.className = "hint warn"; return; }
  h.textContent = (e[1] || v) + (e[2] ? " · " + e[2] : "");
}

function entityControl(f, st) {
  const domains = domainsOf(f);
  const input = el("input", { type: "text", value: f.value, spellcheck: false, autocomplete: "off",
    placeholder: domains.length ? domains[0] + ".…" : "any entity, e.g. sensor.…" });
  input.setAttribute("list", datalistFor(domains.join(",")).id);
  input.setAttribute("autocapitalize", "off");
  const hint = el("div", { className: "hint" });
  const p = { input, hint, domains };
  pickers.push(p);
  let saved = f.value;
  const commit = () => {
    const v = input.value.trim();
    input.value = v;
    updateHint(p);
    if (v !== saved) { const prev = saved; saved = v; save(f.key, v, st, () => { saved = prev; }); }
  };
  input.addEventListener("change", commit);
  input.addEventListener("input", () => updateHint(p));
  input.addEventListener("keydown", (e) => { if (e.key === "Enter") input.blur(); });
  updateHint(p);
  controls.set(f.key, { set(v) { if (document.activeElement !== input) { input.value = v; saved = v; updateHint(p); } } });
  return [el("div", { className: "line" }, [input, st]), hint];
}

function numberControl(f, st) {
  let input;
  if (f.min === -1 && f.max <= 6 && f.step === 1) {
    // Precision fields: -1 means "as Home Assistant reports it".
    input = el("select");
    for (let v = f.min; v <= f.max; v++) input.appendChild(el("option", { value: String(v), text: v < 0 ? "As reported" : v + " decimal" + (v === 1 ? "" : "s") }));
  } else {
    input = el("input", { type: "number", min: f.min, max: f.max, step: f.step });
  }
  input.value = String(f.value);
  let saved = String(f.value);
  input.addEventListener("change", () => {
    const n = Number(input.value);
    if (input.value === "" || !isFinite(n) || n < f.min || n > f.max) { status(st, "Range " + f.min + "–" + f.max, "err"); input.value = saved; return; }
    if (input.value === saved) return;
    const prev = saved; saved = input.value;
    save(f.key, input.value, st, () => { saved = prev; input.value = prev; });
  });
  controls.set(f.key, { set(v) { if (document.activeElement !== input) { input.value = String(v); saved = String(v); } } });
  return [el("div", { className: "line" }, [input, f.unit ? el("span", { className: "unit", text: f.unit }) : null, st])];
}

function selectControl(f, st) {
  const sel = el("select");
  f.options.forEach((o) => sel.appendChild(el("option", { value: o, text: o })));
  sel.value = f.value;
  let saved = f.value;
  sel.addEventListener("change", () => { const prev = saved; saved = sel.value; save(f.key, sel.value, st, () => { saved = prev; sel.value = prev; }); });
  controls.set(f.key, { set(v) { if (document.activeElement !== sel) { sel.value = v; saved = v; } } });
  return [el("div", { className: "line" }, [sel, st])];
}

function switchControl(f, st) {
  const cb = el("input", { type: "checkbox", checked: !!f.value });
  cb.addEventListener("change", () => save(f.key, cb.checked ? "true" : "false", st, () => { cb.checked = !cb.checked; }));
  controls.set(f.key, { set(v) { cb.checked = !!v; } });
  return [el("div", { className: "line" }, [el("label", { className: "switch" }, [cb, el("span")]), st])];
}

function render(state) {
  document.title = state.title + " settings";
  $("#title").textContent = state.title;
  const main = $("#main");
  main.textContent = "";
  const groups = new Map();
  for (const f of state.fields) {
    if (!groups.has(f.group)) {
      const sec = el("section", null, [el("h2", { text: f.group })]);
      groups.set(f.group, sec);
      main.appendChild(sec);
    }
    const id = "f_" + f.key;
    const st = el("span", { className: "st", role: "status" });
    const build = { entity: entityControl, number: numberControl, select: selectControl, switch: switchControl }[f.kind];
    if (!build) continue;
    const parts = build(f, st);
    const first = parts[0].querySelector("input,select");
    if (first) first.id = id;
    const ctl = el("div", { className: "ctl" }, parts);
    if (f.help) ctl.appendChild(el("div", { className: "hint", text: f.help }));
    groups.get(f.group).appendChild(el("div", { className: "row" }, [el("label", { htmlFor: id, text: f.label }), ctl]));
  }
  if (!state.token) showMsg("No ha_token is set in the firmware, so entity lists cannot be loaded from Home Assistant. Entity ids can still be typed in.", false);
}

async function getState() {
  const r = await fetch("/settings/state", { cache: "no-store" });
  if (!r.ok) throw new Error("HTTP " + r.status);
  return r.json();
}

async function refreshValues() {
  try {
    const s = await getState();
    for (const f of s.fields) { const c = controls.get(f.key); if (c) c.set(f.value); }
  } catch (e) {}
}

let polling = false;
async function loadEntities(force) {
  if (polling) return;
  polling = true;
  const btn = $("#refresh"), out = $("#hastatus");
  btn.disabled = true;
  try {
    let url = "/settings/entities" + (force ? "?refresh=1" : "");
    for (let i = 0; i < 90; i++) {
      const r = await fetch(url, { cache: "no-store" });
      const j = await r.json();
      if (j.entities && j.entities.length) {
        entities = j.entities.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
        byId.clear();
        entities.forEach((e) => byId.set(e[0], e));
        fillLists();
      }
      if (j.status === "loading") {
        out.textContent = "Loading entities from Home Assistant…";
        url = "/settings/entities";
        await new Promise((res) => setTimeout(res, 1000));
        continue;
      }
      if (j.status === "error") {
        out.textContent = "Home Assistant: " + j.error + (entities.length ? " (showing the last list)" : "");
      } else {
        out.textContent = entities.length + " entities from Home Assistant";
      }
      return;
    }
    out.textContent = "Home Assistant did not answer in time";
  } catch (e) {
    out.textContent = "Could not load entities: " + e.message;
  } finally {
    btn.disabled = false;
    polling = false;
  }
}

$("#refresh").addEventListener("click", () => loadEntities(true));

(async () => {
  try {
    render(await getState());
  } catch (e) {
    showMsg("Could not read the device settings: " + e.message, true);
    return;
  }
  loadEntities(false);
})();
</script>
</body>
</html>
)HTML";

}  // namespace esp32_dash
