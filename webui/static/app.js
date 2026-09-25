"use strict";

(() => {
  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
  const enc = encodeURIComponent;
  const esc = (value) => String(value ?? "").replace(/[&<>"']/g,
    (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));

  const S = {
    schema: null,
    accounts: [],
    settings: {},
    os: "posix",
    view: "welcome",     // welcome | account | add | settings | help
    prefix: "$",         // command prefix of the last opened account, for the commands page
    id: null,
    tab: "status",
    acc: null,           // last payload of the open account
    form: {},            // current, possibly unsaved, values
    errors: {},
    pendingRestart: null,
    logOffset: -1,
    logPartial: "",
    logTimer: null,
    importResult: null,
    importing: false,
    nav: 0,              // bumped on every navigation, so a late answer cannot pull you back
    tagFilter: "",        // sidebar filter: "" = tous les comptes, sinon un tag exact
  };

  // ------------------------------------------------------------------ api

  async function api(method, path, body) {
    const options = { method, headers: {} };
    if (body instanceof Blob) {
      options.body = body;
      options.headers["Content-Type"] = "application/octet-stream";
    } else if (body !== undefined) {
      options.body = JSON.stringify(body);
      options.headers["Content-Type"] = "application/json";
    }
    const response = await fetch(path, options);
    let data = null;
    try { data = await response.json(); } catch (_) { /* no body */ }
    if (!response.ok) {
      const error = new Error((data && data.error) || `Erreur ${response.status}`);
      error.status = response.status;
      error.data = data;
      throw error;
    }
    return data;
  }

  function toast(message, kind = "") {
    const el = document.createElement("div");
    el.className = "toast " + kind;
    el.textContent = message;
    const list = $("#toasts");
    list.appendChild(el);
    while (list.children.length > 3) list.firstChild.remove(); // never let notifications cover the page
    setTimeout(() => el.remove(), kind === "err" ? 7000 : 3500);
  }

  // -------------------------------------------------------------- helpers

  const accountById = (id) => S.accounts.find((a) => a.id === id);

  // Areas refreshed by polling are only rewritten when their content really changed: replacing a
  // button between the mouse press and release would swallow the click.
  const slots = {};
  const slot = (id, html) => { slots[id] = html; return html; };   // remember what a template just rendered
  function setSlot(id, html) {
    if (slots[id] === html) return;
    slots[id] = html;
    const el = document.getElementById(id);
    if (el) el.innerHTML = html;
  }

  function displayName(account) {
    if (account.alias) return account.alias;
    if (account.igg_id) return `Compte ${account.igg_id}`;
    return account.id;
  }

  function statusKind(status) {
    if (status.state === "running") return "running";
    return status.exit_code ? "failed" : "stopped";
  }

  function fmtDuration(seconds) {
    seconds = Math.max(0, Math.round(seconds));
    if (seconds < 60) return `${seconds} s`;
    if (seconds < 3600) return `${Math.floor(seconds / 60)} min`;
    if (seconds < 86400) return `${Math.floor(seconds / 3600)} h ${Math.floor(seconds % 3600 / 60)} min`;
    return `${Math.floor(seconds / 86400)} j ${Math.floor(seconds % 86400 / 3600)} h`;
  }

  function statusText(status) {
    if (status.state === "running") return `En marche · ${fmtDuration(Date.now() / 1000 - status.since)}`;
    return status.exit_code ? `Arrêté (code ${status.exit_code})` : "Arrêté";
  }

  const HINTS = [
    [/Login refused \d+ times|server error code|rejected by server|Login failed/i,
      "Le serveur refuse la clé d'accès. Elle est probablement remplacée quand le jeu se connecte de nouveau (surtout avec "
      + "le lanceur PC) : refaites une capture pendant que le jeu est ouvert (Ajouter un compte → Capture automatique)."],
    [/UPDATE CLIENT VERSION/i,
      "Le jeu a été mis à jour : la version du client est trop ancienne. Réimportez une capture pour récupérer la nouvelle."],
    [/another device.*not reconnecting/i,
      "Le bot s'est arrêté volontairement : le compte a été utilisé sur un autre appareil et le délai de reconnexion "
      + "est réglé à 0. Redémarrez-le quand vous avez fini de jouer."],
    [/LOGGING FROM ANOTHER DEVICE/i, "Le compte s'est connecté depuis un autre appareil."],
    [/Failed to load config/i, "Le fichier de configuration est invalide. Vérifiez les réglages."],
    [/Failed to connect/i, "Serveur injoignable. Vérifiez votre connexion et l'adresse de la passerelle."],
  ];

  function hintFor(status) {
    if (status.state === "running" || !status.last_log) return "";
    const found = HINTS.find(([pattern]) => pattern.test(status.last_log));
    return found ? found[1] : "";
  }

  const fieldId = (key) => "f_" + key.replace(/\./g, "_");
  const splitShields = (value) => String(value || "").split(",").map((s) => s.trim()).filter(Boolean);
  const splitList = splitShields;
  const warnActive = (f, value) => !!f.warn_if
    && (f.type === "names" ? splitList(value).includes(f.warn_if) : value === f.warn_if);
  const namesHint = (value) => {
    const n = splitList(value).length;
    return n ? `${n} administrateur${n > 1 ? "s" : ""}` : "Aucun administrateur : personne n'a les droits complets.";
  };

  function initialForm(payload) {
    const form = { ...payload.values };
    Object.keys(payload.secrets).forEach((key) => { form[key] = ""; });
    return form;
  }

  const dirtyKeys = () => (S.acc ? Object.keys(S.form).filter((k) => S.form[k] !== S.acc.values[k]) : []);

  function confirmDiscard() {
    return !dirtyKeys().length || confirm("Des modifications ne sont pas enregistrées. Les abandonner ?");
  }

  function sizeHint(text) {
    const match = /^(\d+(?:\.\d+)?)([kKmMbB]?)$/.exec(text.trim());
    if (!match) return { text: "Format invalide (ex. 500K, 20M, 1B)", bad: true };
    const factor = { "": 1, k: 1e3, m: 1e6, b: 1e9 }[match[2].toLowerCase()];
    const value = Math.floor(parseFloat(match[1]) * factor);
    if (value > 4294967295) return { text: "Trop grand (maximum 4 294 967 295)", bad: true };
    return { text: "= " + value.toLocaleString("fr-FR"), bad: false };
  }

  // ---------------------------------------------------------------- shell

  function renderShell() {
    $("#app").innerHTML = `
      <header class="topbar">
        <span class="brand">Lords Mobile Bot<small>Console</small></span>
        <span class="spacer"></span>
        <button class="btn" data-act="help-view">Commandes</button>
        <button class="btn" data-act="settings-view">Paramètres</button>
      </header>
      <div class="layout">
        <aside class="sidebar" id="sidebar" aria-label="Comptes"></aside>
        <main id="main"></main>
      </div>
      <div class="savebar" id="savebar" hidden></div>`;
  }

  function scopedAccounts() {
    return S.tagFilter ? S.accounts.filter((a) => a.tag === S.tagFilter) : S.accounts;
  }

  function renderSidebar() {
    const tags = [...new Set(S.accounts.map((a) => a.tag).filter(Boolean))].sort((a, b) => a.localeCompare(b));
    if (S.tagFilter && !tags.includes(S.tagFilter)) S.tagFilter = ""; // tag renamed/removed: drop a stale filter
    const scope = scopedAccounts();
    const items = scope.map((a) => {
      const kind = statusKind(a.status);
      const current = S.view === "account" && S.id === a.id;
      const sub = a.igg_id ? `IGG ${a.igg_id}` : (a.has_key ? a.id : "identifiants manquants");
      const tagBadge = a.tag ? `<span class="tag-badge">${esc(a.tag)}</span>` : "";
      return `<div class="acc-row"><button class="acc" data-act="open-account" data-id="${esc(a.id)}" ${current ? 'aria-current="true"' : ""}>
        <span class="dot ${kind}" title="${esc(statusText(a.status))}"></span>
        <span class="txt"><div class="name"><span class="name-text">${esc(displayName(a))}</span>${tagBadge}</div><div class="sub">${esc(sub)}</div></span>
      </button><button class="iconbtn acc-edit" data-act="rename-account" data-id="${esc(a.id)}" title="Renommer" aria-label="Renommer ${esc(displayName(a))}">✎</button></div>`;
    }).join("");
    const running = scope.filter((a) => a.status.state === "running").length;
    const multi = S.accounts.length > 1;
    const filterHtml = tags.length ? `<div class="side-filter">
      <select id="tag-filter" aria-label="Filtrer par tag">
        <option value="" ${S.tagFilter ? "" : "selected"}>Tous les comptes (${S.accounts.length})</option>
        ${tags.map((t) => `<option value="${esc(t)}" ${S.tagFilter === t ? "selected" : ""}>${esc(t)} (${S.accounts.filter((a) => a.tag === t).length})</option>`).join("")}
      </select>
    </div>` : "";
    const bulkLabel = S.tagFilter ? esc(S.tagFilter) : "Tout";
    const sidebarHtml = `
      <div class="side-title">Comptes</div>
      ${filterHtml}
      ${items || '<p class="help" style="padding:0 8px">Aucun compte pour ce filtre.</p>'}
      <div class="side-actions"><button class="btn wide" data-act="add-view">＋ Ajouter un compte</button></div>
      ${multi ? `<div class="side-foot">
        <button class="btn wide" data-act="start-all">Démarrer : ${bulkLabel}</button>
        <button class="btn wide" data-act="stop-all" ${running ? "" : "disabled"}>Arrêter : ${bulkLabel}</button>
      </div>` : ""}`;
    setSlot("sidebar", sidebarHtml);
  }

  function renderMain() {
    stopLogs();
    clearInterval(S.capTimer);
    clearTimeout(S.capPoll);
    const main = $("#main");
    if (S.view === "account" && S.acc) {
      main.innerHTML = accountView();
      applyDerived();
      if (S.tab === "logs") startLogs();
      if (S.tab === "status" || S.tab === "chat") startGame();
      if (S.tab === "bank") startBank();
    } else if (S.view === "add") {
      main.innerHTML = addView();
      S.capTimer = setInterval(() => {
        const clock = $("#cap-timer");
        if (clock && S.capture && S.capture.since) clock.textContent = fmtClock(Date.now() / 1000 - S.capture.since);
      }, 1000);
      pollCapture();
    } else if (S.view === "settings") {
      main.innerHTML = settingsView();
    } else if (S.view === "help") {
      main.innerHTML = helpView();
    } else {
      main.innerHTML = welcomeView();
    }
    renderSavebar();
  }

  function render() {
    renderSidebar();
    renderMain();
  }

  // ---------------------------------------------------------------- views

  function welcomeView() {
    return `<div class="empty">
      <h1>Bienvenue</h1>
      <p>Aucun compte n'est encore configuré. Importez une capture réseau du jeu pendant que vous vous connectez :
      la console en extrait toute seule la clé d'accès, la version du client et la passerelle.</p>
      <button class="btn primary" data-act="add-view">Ajouter un compte</button>
    </div>`;
  }

  function accountView() {
    const a = S.acc;
    const summary = accountById(a.id) || { id: a.id, alias: a.alias, igg_id: a.values["account.igg_id"] };
    const badgeFor = (categories) => {
      const keys = categories.flatMap((c) => c.fields.map((f) => f.key));
      const dirty = keys.filter((k) => S.form[k] !== a.values[k]).length;
      const bad = keys.filter((k) => S.errors[k]).length;
      return bad ? `<span class="count bad">${bad}</span>` : (dirty ? `<span class="count">${dirty}</span>` : "");
    };
    const everyday = S.schema.categories.filter((c) => !c.technical);
    const technical = S.schema.categories.filter((c) => c.technical);
    const tabs = everyday.map((c) => `<button class="tab" role="tab" data-act="tab" data-tab="${c.id}" aria-selected="${S.tab === c.id}">${esc(c.label)}${badgeFor([c])}</button>`).join("");
    const technicalTab = `<button class="tab tab-technical" role="tab" data-act="tab" data-tab="technical" aria-selected="${S.tab === "technical"}" title="Identifiants, serveur, version : rarement utile">⚙ Technique${badgeFor(technical)}</button>`;
    const statusTab = `<button class="tab" role="tab" data-act="tab" data-tab="status" aria-selected="${S.tab === "status"}">Statut</button>`;
    const chatTab = `<button class="tab" role="tab" data-act="tab" data-tab="chat" aria-selected="${S.tab === "chat"}">Chat de guilde</button>`;
    const bankTab = `<button class="tab" role="tab" data-act="tab" data-tab="bank" aria-selected="${S.tab === "bank"}">Banque de guilde</button>`;
    const logsTab = `<button class="tab" role="tab" data-act="tab" data-tab="logs" aria-selected="${S.tab === "logs"}">Journal</button>`;
    const category = S.schema.categories.find((c) => c.id === S.tab);
    return `
      <div class="head">
        <div class="title">
          <h1 id="acc-title">${esc(displayName(summary))} ${summary.tag ? `<span class="tag-badge">${esc(summary.tag)}</span>` : ""} <button class="iconbtn" data-act="rename" title="Renommer le compte" aria-label="Renommer le compte">✎</button></h1>
          <div class="subtitle mono">accounts/${esc(a.id)}.cfg</div>
        </div>
        <div class="actions" id="ctrl"><span id="ctrl-chip">${slot("ctrl-chip", chipHtml())}</span><span id="ctrl-btns">${slot("ctrl-btns", btnsHtml())}</span></div>
      </div>
      <div id="notes">${slot("notes", notesHtml())}</div>
      <div class="tabs" role="tablist">${statusTab}${chatTab}${bankTab}${tabs}${logsTab}${technicalTab}</div>
      ${S.tab === "logs" ? logsView() : S.tab === "status" ? `<div id="game">${gameHtml(S.game)}</div>`
        : S.tab === "chat" ? chatView() : S.tab === "bank" ? bankView()
        : S.tab === "technical" ? technicalView(technical) : categoryView(category)}`;
  }

  function chipHtml() {
    const status = S.acc.status;
    const kind = statusKind(status);
    return `<span class="chip ${kind}"><span class="dot ${kind}"></span>${esc(statusText(status))}</span>`;
  }

  function btnsHtml() {
    if (S.acc.status.state === "running") {
      return `<button class="btn" data-act="restart">Redémarrer</button><button class="btn danger" data-act="stop">Arrêter</button>`;
    }
    return `<button class="btn ok" data-act="start">Démarrer</button>`;
  }

  function notesHtml() {
    const status = S.acc.status;
    let html = "";
    const hint = hintFor(status);
    if (hint) {
      html += `<div class="notice err"><p><strong>${esc(hint)}</strong></p><p class="mono">${esc(status.last_log)}</p></div>`;
    }
    if (S.pendingRestart === S.acc.id && status.state === "running") {
      html += `<div class="notice warn"><p>Les réglages enregistrés s'appliqueront au prochain démarrage du bot.
        <button class="btn small" data-act="restart">Redémarrer maintenant</button></p></div>`;
    }
    return html;
  }

  function technicalView(categories) {
    return `<div class="notice warn"><p><strong>Réglages techniques.</strong> Ils viennent de la capture du jeu et sont remplis
      automatiquement : à modifier seulement si vous savez pourquoi (nouvelle clé d'accès, changement de serveur ou de version du jeu).</p></div>`
      + categories.map((c) => `<h2 class="subhead">${esc(c.label)}</h2>${categoryView(c)}`).join("") + dangerHtml();
  }

  function categoryView(category) {
    let html = `<p class="desc">${esc(category.description)}</p>`;
    const blocks = [];
    category.fields.forEach((f) => {
      const group = f.group || "";
      const last = blocks[blocks.length - 1];
      if (last && last.group === group) last.fields.push(f); else blocks.push({ group, fields: [f] });
    });
    blocks.forEach((block) => {
      const grid = block.fields.length > 2 && block.fields.every((f) => f.type === "bool" || f.type === "size");
      const master = !block.group && block.fields.length === 1 && block.fields[0].type === "bool"
        && category.fields.some((f) => f.depends === block.fields[0].key);
      html += `<section class="card${grid ? " grid" : ""}${master ? " master" : ""}">
        ${block.group ? `<h3>${esc(block.group)}</h3>` : ""}
        <div class="fields">${block.fields.map(fieldHtml).join("")}</div></section>`;
    });
    if (category.id === "advanced") html += extraHtml();
    return html;
  }

  function extraHtml() {
    const entries = Object.entries(S.acc.extra || {});
    if (!entries.length) return "";
    const rows = entries.map(([k, v]) => `<div class="mono">${esc(k)} = ${/key|token|secret|pass/i.test(k) ? "••••••" : esc(v)}</div>`).join("");
    return `<section class="card"><h3>Autres réglages du fichier</h3>
      <p class="help">Ces réglages ne sont pas gérés par la console. Ils sont conservés tels quels.</p>${rows}</section>`;
  }

  function dangerHtml() {
    const running = S.acc.status.state === "running";
    return `<section class="card"><h3>Zone dangereuse</h3>
      <p class="help">Supprime la configuration de ce compte (les identifiants seront perdus).</p>
      <button class="btn danger" data-act="delete-account" ${running ? "disabled" : ""}>Supprimer ce compte</button>
      ${running ? '<p class="help">Arrêtez d\'abord le bot.</p>' : ""}</section>`;
  }

  function fieldHtml(f) {
    const key = f.key, id = fieldId(key), value = S.form[key], error = S.errors[key];
    const isDefault = S.acc.defaults_used.includes(key) && value === S.acc.values[key];
    const badge = (f.inactive ? '<span class="badge inactive">pas encore actif</span>' : "")
      + (isDefault ? '<span class="badge">par défaut</span>' : "");
    const help = (f.help ? `<p class="help">${esc(f.help)}</p>` : "")
      + (f.inactive ? `<p class="help">${esc(f.inactive)}</p>` : "");
    const tail = `<p class="error" data-err="${esc(key)}" ${error ? "" : "hidden"}>${esc(error || "")}</p>
      <p class="warnmsg" data-warn="${esc(key)}" ${warnActive(f, value) ? "" : "hidden"}>${esc(f.warn || "")}</p>`;
    const dep = f.depends ? ` data-depends="${esc(f.depends)}"` : "";
    const invalid = error ? " invalid" : "";

    if (f.type === "bool") {
      return `<div class="field field-bool"${dep}>
        <div class="fl"><label class="lbl" for="${id}">${esc(f.label)} ${badge}</label>${help}</div>
        <label class="switch"><input type="checkbox" id="${id}" data-key="${esc(key)}" ${value === "true" ? "checked" : ""}><span class="slider"></span></label>
      </div>`;
    }

    let control;
    if (f.type === "int") {
      control = `<div class="inline"><input type="number" id="${id}" class="${invalid}" data-key="${esc(key)}" inputmode="numeric"
        min="${f.min ?? 0}" max="${f.max ?? ""}" step="1" value="${esc(value)}">${f.unit ? `<span class="unit">${esc(f.unit)}</span>` : ""}</div>`;
    } else if (f.type === "secret") {
      const info = S.acc.secrets[key];
      const placeholder = info.set
        ? `Clé enregistrée (${info.length} caractères${info.preview ? ", " + info.preview : ""}). Laisser vide pour la conserver.`
        : "Aucune clé enregistrée";
      control = `<input type="password" id="${id}" class="${invalid}" data-key="${esc(key)}" autocomplete="new-password" spellcheck="false"
        placeholder="${esc(placeholder)}" value="${esc(value)}">`;
    } else if (f.type === "select") {
      const options = f.options.map(([v, label]) => [v, label]);
      if (!options.some(([v]) => v === value)) options.push([value, `Autre (${value})`]);
      control = `<select id="${id}" class="${invalid}" data-key="${esc(key)}">${options.map(([v, label]) =>
        `<option value="${esc(v)}" ${v === value ? "selected" : ""}>${esc(label)}</option>`).join("")}</select>`;
    } else if (f.type === "size") {
      const hint = sizeHint(value);
      control = `<input type="text" id="${id}" class="${invalid}" data-key="${esc(key)}" spellcheck="false" value="${esc(value)}">
        <span class="hint ${hint.bad ? "bad" : ""}" data-hint="${esc(key)}">${esc(hint.text)}</span>`;
    } else if (f.type === "shields" || f.type === "antiscout") {
      control = shieldsHtml(f);
    } else if (f.type === "channels") {
      control = channelsHtml(f);
    } else if (f.type === "names") {
      control = `<input type="text" id="${id}" class="${invalid}" data-key="${esc(key)}" spellcheck="false" placeholder="pseudo1, pseudo2" value="${esc(value)}">
        <span class="hint" data-names-hint="${esc(key)}">${esc(namesHint(value))}</span>`;
    } else {
      control = `<input type="text" id="${id}" class="${invalid}" data-key="${esc(key)}" spellcheck="false"
        ${f.maxlen ? `maxlength="${f.maxlen}"` : ""} value="${esc(value)}">`;
    }
    return `<div class="field"${dep}><label class="lbl" for="${id}">${esc(f.label)} ${badge}</label>${control}${help}${tail}</div>`;
  }

  function shieldsHtml(f) {
    const key = f.key, current = splitShields(S.form[key]);
    const list = f.type === "antiscout" ? S.schema.antiscout : S.schema.shields;
    const addLabel = f.type === "antiscout" ? "Ajouter un objet anti-espionnage…" : "Ajouter un bouclier…";
    const label = (name) => (list.find(([n]) => n === name) || [name, name])[1];
    const rows = current.map((name, i) => `<div class="shield-row">
      <span class="n">${i + 1}.</span><span class="l">${esc(label(name))}</span>
      <button class="iconbtn" data-act="shield-up" data-key="${esc(key)}" data-i="${i}" ${i === 0 ? "disabled" : ""} aria-label="Monter">↑</button>
      <button class="iconbtn" data-act="shield-down" data-key="${esc(key)}" data-i="${i}" ${i === current.length - 1 ? "disabled" : ""} aria-label="Descendre">↓</button>
      <button class="iconbtn" data-act="shield-del" data-key="${esc(key)}" data-i="${i}" aria-label="Retirer">✕</button></div>`).join("");
    const rest = list.filter(([name]) => !current.includes(name));
    const add = rest.length ? `<select data-shield-add="${esc(key)}" aria-label="${esc(addLabel)}">
      <option value="">${esc(addLabel)}</option>${rest.map(([name, text]) => `<option value="${esc(name)}">${esc(text)}</option>`).join("")}</select>` : "";
    return `<div class="shields" id="${fieldId(key)}" data-shields="${esc(key)}">${rows}${add}</div>`;
  }

  function channelsHtml(f) {
    const current = splitList(S.form[f.key]);
    return `<div class="channels" id="${fieldId(f.key)}" data-channels="${esc(f.key)}">${f.options.map(([value, label]) =>
      `<label class="radio"><input type="checkbox" data-channel="${esc(value)}" data-channel-key="${esc(f.key)}" ${current.includes(value) ? "checked" : ""}> ${esc(label)}</label>`).join("")}</div>`;
  }

  function codeLine(text) {
    return `<div class="code inline-code"><pre>${esc(text)}</pre><button class="btn small" data-act="copy" data-text="${esc(text)}">Copier</button></div>`;
  }

  function helpView() {
    const commands = S.schema.commands || [];
    const groups = [];
    commands.forEach((c) => {
      let group = groups.find((g) => g.name === c.group);
      if (!group) { group = { name: c.group, items: [] }; groups.push(group); }
      group.items.push(c);
    });
    const p = S.prefix || "$";
    const cards = groups.map((g) => `<section class="card"><h3>${esc(g.name)}</h3><div class="fields">${g.items.map((c) => `
      <div class="cmd">
        <div class="cmd-head"><code class="usage">${esc(p + c.usage)}</code><span class="badge">${esc(c.who)}</span></div>
        <p class="help">${esc(c.summary)}</p>
        ${(c.details || []).length ? `<ul class="help">${c.details.map((d) => `<li>${esc(d)}</li>`).join("")}</ul>` : ""}
        <div class="cmd-example"><span class="help">Exemple :</span> ${codeLine(p + c.example)}</div>
      </div>`).join("")}</div></section>`).join("");
    return `
      <div class="head"><div class="title"><h1>Commandes en jeu</h1>
        <div class="subtitle">Ce que le bot comprend, comment l'utiliser et qui peut le faire.</div></div></div>
      <div class="notice">
        <p>Écrivez la commande dans un canal accepté par le bot (réglage <strong>Commandes → Canaux de réception</strong>).
        Le bot répond dans le canal de réponse choisi. Le préfixe actuel est <code>${esc(p)}</code>.</p>
        <p>En jeu, <code>${esc(p)}help</code> donne la liste adaptée aux droits de la personne qui la demande.</p>
      </div>
      ${cards}`;
  }

  function logsView() {
    return `<div class="toolbar">
      <label class="inline"><input type="checkbox" id="log-follow" checked style="width:auto;min-height:0"> Suivre</label>
      <button class="btn small" data-act="clear-log">Effacer l'affichage</button>
      <span class="spacer"></span>
      <span class="help">Journal complet : logs/${esc(S.acc.id)}.log</span>
    </div>
    <pre class="log" id="log" tabindex="0" aria-label="Journal du bot" aria-live="off"></pre>`;
  }

  const CAPTURE_STEPS_WINDOWS = `pktmon start --capture --pkt-size 0 -f capture.etl
# lancez le jeu et attendez d'être en jeu, puis :
pktmon stop
pktmon etl2pcap capture.etl -o capture.pcapng`;

  function codeBlock(text) {
    return `<div class="code"><pre>${esc(text)}</pre><button class="btn small" data-act="copy" data-text="${esc(text)}">Copier</button></div>`;
  }

  function resultsHtml() {
    if (!S.importResult) return "";
    return `<section class="card"><h2>Comptes importés</h2>${S.importResult.map((r) => `<div class="result">
      <div class="grow"><strong>Compte ${esc(r.id)}</strong> <span class="badge">${r.created ? "créé" : "mis à jour"}</span>
        <div class="help">Client ${esc(r.version)}${r.gateway ? " · passerelle " + esc(r.gateway) : ""} · clé de ${r.key_length} caractères</div></div>
      ${r.remote ? `<span class="badge">envoyé au serveur</span>` : `<button class="btn small" data-act="open-account" data-id="${esc(r.id)}">Ouvrir</button>`}</div>`).join("")}
      <p class="help">Donnez-leur un nom (Bank, Filler…) avec le crayon ✎ à côté du nom du compte.</p></section>`;
  }

  const fmtClock = (seconds) => {
    seconds = Math.max(0, Math.round(seconds));
    return `${String(Math.floor(seconds / 60)).padStart(2, "0")}:${String(seconds % 60).padStart(2, "0")}`;
  };

  function captureCard() {
    const c = S.capture || { state: "idle", available: false };
    let body;
    if (!c.available) {
      body = `<p class="help">La capture automatique n'est disponible que sous Windows. Faites la capture avec Wireshark
        (ou tcpdump), puis importez le fichier avec la carte « Importer une capture réseau » ci-dessous.</p>`;
    } else if (c.state === "starting") {
      body = `<p><span class="spin"></span> En attente de l'autorisation Windows… Acceptez la fenêtre
        « Contrôle de compte d'utilisateur » : elle porte le nom <strong>Windows PowerShell</strong> (c'est lui qui lance la capture)
        et peut se cacher derrière d'autres fenêtres.</p>
        <button class="btn" data-act="capture-cancel">Annuler</button>`;
    } else if (c.state === "recording") {
      body = `<p><span class="rec"></span> <strong>Capture en cours</strong> · <span id="cap-timer">${fmtClock(Date.now() / 1000 - c.since)}</span></p>
        <p class="help">Lancez maintenant le jeu et connectez-vous au compte. Pour ajouter plusieurs comptes : déconnectez-vous
        dans le jeu, puis connectez-vous au suivant. Quand vous avez fini, cliquez sur <strong>Terminer</strong>.</p>
        <div class="inline"><button class="btn primary" data-act="capture-stop">Terminer et importer</button>
        <button class="btn" data-act="capture-cancel">Annuler</button></div>`;
    } else if (c.state === "processing") {
      body = `<p><span class="spin"></span> Arrêt de la capture et analyse…</p>`;
    } else {
      body = `${c.state === "error" && c.message ? `<div class="notice err"><p>${esc(c.message)}</p></div>` : ""}
        <p class="help">Recommandé. La console lance la capture réseau pour vous, vous vous connectez à vos comptes dans le jeu,
        puis elle importe tout et supprime la capture.</p>
        <ol><li>Fermez le jeu.</li>
          <li>Cliquez sur <strong>Démarrer</strong> : Windows demande l'autorisation administrateur.</li>
          <li>Lancez le jeu et connectez-vous au compte (pour plusieurs comptes, déconnectez-vous dans le jeu puis connectez-vous au suivant).</li>
          <li>Cliquez sur <strong>Terminer et importer</strong>.</li></ol>
        <label class="inline"><input type="checkbox" id="cap-all" style="width:auto;min-height:0">
          Capturer tout le trafic TCP (plus lourd ; à essayer si aucun login n'est trouvé)</label>
        <p class="help">Par défaut seul le trafic vers le port 5999 est enregistré. Le fichier reste dans un dossier temporaire privé et est
        supprimé dès l'import ; la console ne garde que les comptes.</p>
        <button class="btn primary" data-act="capture-start">Démarrer la capture</button>`;
    }
    return `<section class="card" id="capture-card"><h2>Capturer depuis cet ordinateur</h2>${body}</section>`;
  }

  function addView() {
    const copyOptions = S.accounts.map((a) => `<option value="${esc(a.id)}">${esc(displayName(a))}</option>`).join("");
    return `
      <div class="head"><div class="title"><h1>Ajouter un compte</h1>
        <div class="subtitle">Chaque compte a son fichier de configuration, son bot et son journal.</div></div></div>
      ${resultsHtml()}
      ${captureCard()}
      <div class="two">
        <section class="card">
          <h2>Importer une capture réseau</h2>
          <p class="help">Si vous avez déjà un fichier <code>.pcap</code> / <code>.pcapng</code> (Wireshark, autre machine…). La console retrouve toute seule le
            compte, la clé d'accès, la version du client et la passerelle. Un compte qui existe déjà voit seulement ses identifiants
            mis à jour ; vos autres réglages sont conservés.</p>
          <label class="drop" id="drop"><input type="file" id="file" accept=".pcap,.pcapng,.cap">
            ${S.importing ? '<span class="spin"></span><strong>Analyse de la capture…</strong>'
              : "<strong>Choisir la capture</strong><span>ou déposer le fichier ici (.pcap, .pcapng)</span>"}</label>
          <details><summary>Faire la capture à la main</summary>
            <ol>
              <li>Fermez complètement le jeu.</li>
              <li>${S.os === "windows" ? "Ouvrez PowerShell <strong>en administrateur</strong> et lancez :" : "Démarrez une capture du trafic TCP (Wireshark, tcpdump…), puis lancez le jeu."}
                ${S.os === "windows" ? codeBlock(CAPTURE_STEPS_WINDOWS) : ""}</li>
              <li>Démarrez la capture <strong>avant</strong> d'ouvrir le jeu, et arrêtez-la seulement une fois en jeu.</li>
              <li>Importez le fichier ici, puis <strong>supprimez-le</strong> : il contient la clé d'accès du compte.</li>
            </ol>
          </details>
        </section>
        <section class="card">
          <h2>Créer un compte vide</h2>
          <p class="help">Pour saisir les identifiants à la main, ou préparer un compte avant d'importer sa capture.</p>
          <div class="fields">
            <div class="field"><label class="lbl" for="new-name">Nom du compte</label>
              <input type="text" id="new-name" maxlength="40" placeholder="ex. Bank, Filler, Farm" spellcheck="false">
              <p class="help">C'est le nom affiché dans la console. Vous pourrez le changer à tout moment.</p></div>
            ${S.accounts.length ? `<div class="field"><label class="lbl" for="new-copy">Copier les réglages de</label>
              <select id="new-copy"><option value="">Réglages par défaut</option>${copyOptions}</select>
              <p class="help">Les identifiants ne sont jamais copiés.</p></div>` : ""}
            <button class="btn primary" data-act="create-empty">Créer le compte</button>
          </div>
        </section>
      </div>`;
  }

  function settingsView() {
    const s = S.settings;
    const detected = (s.client_detected || []).map((p) => `<div class="radio">
      <button class="btn small" data-act="use-client" data-path="${esc(p)}">Utiliser</button><span class="mono">${esc(p)}</span></div>`).join("");
    return `
      <div class="head"><div class="title"><h1>Paramètres</h1></div></div>
      <section class="card"><h3>Exécutable du bot</h3>
        <p class="help">${s.client_used ? `Utilisé actuellement : <span class="mono">${esc(s.client_used)}</span>`
          : "<strong>Aucun exécutable trouvé.</strong> Compilez le bot (build.bat) ou indiquez son chemin."}</p>
        <div class="fields">
          <div class="field"><label class="lbl" for="set-client">Chemin personnalisé</label>
            <input type="text" id="set-client" spellcheck="false" placeholder="Laisser vide pour la détection automatique" value="${esc(s.client_path || "")}">
            <p class="error" id="set-client-err" hidden></p></div>
          ${detected ? `<div><div class="help">Détectés :</div>${detected}</div>` : ""}
        </div>
      </section>
      <section class="card"><h3>Plusieurs comptes</h3>
        <div class="fields"><div class="field"><label class="lbl" for="set-stagger">Délai entre deux démarrages</label>
          <div class="inline"><input type="number" id="set-stagger" min="0" max="600" value="${esc(s.stagger)}"><span class="unit">secondes</span></div>
          <p class="help">« Tout démarrer » lance les comptes un par un avec ce délai, pour ne pas se connecter tous en même temps.</p>
          <p class="error" id="set-stagger-err" hidden></p></div></div>
      </section>
      <section class="card"><h3>Serveur distant</h3>
        <p class="help">Le jeu est sur cet ordinateur, les bots sont sur un serveur : indiquez la console du serveur et
        <strong>toute capture faite ici (automatique ou importée) lui sera envoyée</strong> au lieu d'être gardée sur ce
        PC. Laissez vide pour tout garder ici.</p>
        <div class="fields">
          <div class="field"><label class="lbl" for="set-remote_url">Adresse de la console du serveur</label>
            <input type="text" id="set-remote_url" spellcheck="false" placeholder="https://bot.exemple.com" value="${esc(s.remote_url || "")}">
            <p class="error" id="set-remote_url-err" hidden></p></div>
          <div class="field"><label class="lbl" for="set-remote_token">Jeton de la console du serveur (optionnel)</label>
            <input type="password" id="set-remote_token" autocomplete="off" placeholder="${s.remote_token_set ? "Enregistré : laisser vide pour le garder" : "Laissez vide si la console distante n'en demande pas"}">
            <p class="help">La console n'a plus de jeton d'accès par défaut : ce champ ne sert que si la console distante est protégée autrement (proxy, etc.) et attend un en-tête <span class="mono">X-Token</span>.</p></div>
        </div>
      </section>
      <section class="card"><h3>Sauvegarde et transfert des comptes</h3>
        <p class="help">Tout ce qui décrit vos comptes (configurations, <strong>clés d'accès</strong>, administrateurs ajoutés
        en jeu, noms) tient dans un fichier. Il sert de sauvegarde, ou à installer ces comptes sur un autre serveur.
        Gardez-le comme un mot de passe.</p>
        <div class="toolbar">
          <button class="btn" data-act="data-export">Télécharger une sauvegarde</button>
          <label class="btn" style="cursor:pointer">Restaurer une sauvegarde<input type="file" id="data-file" accept=".json" hidden></label>
          <button class="btn" data-act="data-push" ${s.remote_url ? "" : "disabled title=\"Configurez d'abord le serveur distant\""}>Envoyer mes comptes au serveur</button>
        </div>
      </section>
      <button class="btn primary" data-act="save-settings">Enregistrer les paramètres</button>`;
  }

  // ------------------------------------------------- live updates (no re-render)

  function applyDerived() {
    $$("[data-depends]").forEach((el) => {
      const off = S.form[el.dataset.depends] !== "true";
      el.classList.toggle("is-off", off);
      $$("input, select, button", el).forEach((control) => { control.disabled = off; });
    });
    $$("[data-hint]").forEach((el) => {
      const hint = sizeHint(S.form[el.dataset.hint] || "");
      el.textContent = hint.text;
      el.classList.toggle("bad", hint.bad);
    });
    $$("[data-names-hint]").forEach((el) => { el.textContent = namesHint(S.form[el.dataset.namesHint]); });
    S.schema.categories.forEach((c) => c.fields.forEach((f) => {
      if (!f.warn_if) return;
      const el = $(`[data-warn="${f.key}"]`);
      if (el) el.hidden = !warnActive(f, S.form[f.key]);
    }));
  }

  function refreshTabCounts() {
    if (!S.acc) return;
    const groups = {};
    S.schema.categories.forEach((c) => { (groups[c.technical ? "technical" : c.id] ||= []).push(c); });
    Object.entries(groups).forEach(([id, categories]) => {
      const tab = $(`.tab[data-tab="${id}"]`);
      if (!tab) return;
      const keys = categories.flatMap((c) => c.fields.map((f) => f.key));
      const dirty = keys.filter((k) => S.form[k] !== S.acc.values[k]).length;
      const bad = keys.filter((k) => S.errors[k]).length;
      const old = $(".count", tab);
      if (old) old.remove();
      if (bad || dirty) {
        const badge = document.createElement("span");
        badge.className = "count" + (bad ? " bad" : "");
        badge.textContent = bad || dirty;
        tab.appendChild(badge);
      }
    });
  }

  function renderSavebar() {
    const bar = $("#savebar");
    const count = S.view === "account" ? dirtyKeys().length : 0;
    if (!count) { bar.hidden = true; setSlot("savebar", ""); return; }
    const running = S.acc.status.state === "running";
    setSlot("savebar", `<strong>${count} modification${count > 1 ? "s" : ""} non enregistrée${count > 1 ? "s" : ""}</strong>
      <span class="spacer"></span>
      <button class="btn" data-act="discard">Annuler</button>
      <button class="btn ${running ? "" : "primary"}" data-act="save">Enregistrer</button>
      ${running ? '<button class="btn primary" data-act="save-restart">Enregistrer et redémarrer</button>' : ""}`);
    bar.hidden = false;
  }

  function refreshControls() {
    if (S.view !== "account" || !S.acc) return;
    setSlot("ctrl-chip", chipHtml());
    setSlot("ctrl-btns", btnsHtml());
    setSlot("notes", notesHtml());
    renderSavebar();
  }

  // ------------------------------------------------------------------ logs

  function stopLogs() { clearTimeout(S.logTimer); S.logTimer = null; clearTimeout(S.gameTimer); clearInterval(S.gameTick); S.gameTimer = S.gameTick = null; clearTimeout(S.bankTimer); S.bankTimer = null; }

  function startLogs() {
    S.logOffset = -1;
    S.logPartial = "";
    pollLogs();
  }

  function appendLog(text, replace) {
    const box = $("#log");
    if (!box) return;
    if (replace) { box.textContent = ""; S.logPartial = ""; }
    const data = S.logPartial + text;
    const parts = data.split("\n");
    S.logPartial = parts.pop();
    const fragment = document.createDocumentFragment();
    parts.forEach((line) => {
      const span = document.createElement("span");
      const match = /^\[(ERROR|WARN|INFO|DEBUG)/.exec(line);
      span.className = match ? match[1] : (line.startsWith("---") ? "sep" : "");
      span.textContent = line + "\n";
      fragment.appendChild(span);
    });
    box.appendChild(fragment);
    while (box.childNodes.length > 3000) box.removeChild(box.firstChild);
    const follow = $("#log-follow");
    if (!follow || follow.checked) box.scrollTop = box.scrollHeight;
  }

  async function pollLogs() {
    clearTimeout(S.logTimer);
    if (S.view !== "account" || S.tab !== "logs" || !S.acc) return;
    const id = S.id;
    try {
      const result = await api("GET", `/api/accounts/${enc(id)}/logs?offset=${S.logOffset}`);
      if (id === S.id && S.tab === "logs" && S.view === "account") {
        appendLog(result.text, S.logOffset < 0 || result.offset < S.logOffset);
        S.logOffset = result.offset;
        S.acc.status = result.status;
      }
    } catch (error) {
      if (error.status === 401) return authProblem();
    }
    S.logTimer = setTimeout(pollLogs, 1500);
  }

  // ------------------------------------------------------------ game status

  const SHIELDS = { 1146: "4 h", 1051: "8 h", 1462: "12 h", 1052: "1 jour", 1053: "3 jours", 1287: "7 jours", 1288: "14 jours" };
  const RSS = [["food", "Nourriture"], ["rock", "Pierre"], ["wood", "Bois"], ["ore", "Minerai"], ["gold", "Or"]];
  const RANKS = ["Membre", "R1", "R2", "R3", "R4", "R5"];
  const num = (n) => Number(n).toLocaleString("fr-FR");
  const short = (n) => n >= 1e9 ? (n / 1e9).toFixed(2) + " B" : n >= 1e6 ? (n / 1e6).toFixed(2) + " M" : n >= 1e4 ? (n / 1e3).toFixed(1) + " K" : String(n);

  // Small inline icons (no external file, work offline and in both themes).
  const ICONS = {
    food: `<path d="M12 22V8" stroke="#8a6a1f" stroke-width="1.6" stroke-linecap="round" fill="none"/><g fill="#f0c53d" stroke="#b8891a" stroke-width=".8"><ellipse cx="8.2" cy="9" rx="2" ry="3.2" transform="rotate(-25 8.2 9)"/><ellipse cx="15.8" cy="9" rx="2" ry="3.2" transform="rotate(25 15.8 9)"/><ellipse cx="8.6" cy="14.4" rx="2" ry="3.2" transform="rotate(-25 8.6 14.4)"/><ellipse cx="15.4" cy="14.4" rx="2" ry="3.2" transform="rotate(25 15.4 14.4)"/><ellipse cx="12" cy="5" rx="1.9" ry="3.1"/></g>`,
    wood: `<rect x="2.5" y="8" width="19" height="9" rx="4.5" fill="#a86a32" stroke="#6b3f18" stroke-width="1"/><ellipse cx="18" cy="12.5" rx="3.2" ry="4.5" fill="#e0b070" stroke="#6b3f18" stroke-width="1"/><ellipse cx="18" cy="12.5" rx="1.2" ry="2" fill="none" stroke="#a86a32" stroke-width=".9"/><path d="M5 10.5h8M5 14h9" stroke="#7d4b22" stroke-width=".9" stroke-linecap="round"/>`,
    rock: `<path d="M3 19 8 8l5 4 3-6 5 13z" fill="#9aa3b2" stroke="#5c6473" stroke-width="1" stroke-linejoin="round"/><path d="M8 8l2 6-7 5zM16 6l-2 8 7 5z" fill="#c4cad6" opacity=".55"/>`,
    ore: `<path d="M12 2.5 20 8v8l-8 5.5L4 16V8z" fill="#5b7fa8" stroke="#2e4a6b" stroke-width="1" stroke-linejoin="round"/><path d="M12 2.5 20 8l-8 4L4 8z" fill="#9dbbdc"/><path d="M12 12v9.5" stroke="#2e4a6b" stroke-width=".9"/><path d="M6.5 9.5l1.6.9" stroke="#fff" stroke-width="1" stroke-linecap="round" opacity=".7"/>`,
    gold: `<circle cx="12" cy="12" r="9.5" fill="#f2b81c" stroke="#a87a08" stroke-width="1.2"/><circle cx="12" cy="12" r="6.6" fill="none" stroke="#b8860b" stroke-width="1"/><path d="M12 7.5v9M9.6 10.2c0-1.3 1-1.9 2.4-1.9s2.4.6 2.4 1.7-1 1.5-2.4 1.9-2.4.7-2.4 1.9 1 1.8 2.4 1.8 2.4-.6 2.4-1.9" fill="none" stroke="#8a6206" stroke-width="1.1" stroke-linecap="round"/>`,
    gems: `<path d="M7 3.5h10l4 5-9 12.5L3 8.5z" fill="#37b4e8" stroke="#1a6f96" stroke-width="1" stroke-linejoin="round"/><path d="M3 8.5h18M8.5 8.5 12 21l3.5-12.5M7 3.5l1.5 5M17 3.5l-1.5 5" fill="none" stroke="#e8f8ff" stroke-width=".9" opacity=".8"/>`,
    shield: `<path d="M12 2.5 20 5.5v6.2c0 5-3.4 8.4-8 10-4.6-1.6-8-5-8-10V5.5z" fill="#3b6fd6" stroke="#1f3f8f" stroke-width="1.2" stroke-linejoin="round"/><path d="M12 5 17.5 7v4.7c0 3.4-2.2 6-5.5 7.3z" fill="#7fa6ff" opacity=".7"/>`,
    power: `<path d="M13.5 2 5 13.5h6L9.5 22 19 9.5h-6.2z" fill="#f2b81c" stroke="#a87a08" stroke-width="1" stroke-linejoin="round"/>`,
    kills: `<path d="M12 3C7.6 3 4.5 6 4.5 10c0 2.3 1 3.7 2.5 4.7V18h10v-3.3c1.5-1 2.5-2.4 2.5-4.7 0-4-3.1-7-7.5-7z" fill="#e6e8ee" stroke="#6b7385" stroke-width="1.1" stroke-linejoin="round"/><circle cx="9" cy="10.5" r="1.9" fill="#3a4152"/><circle cx="15" cy="10.5" r="1.9" fill="#3a4152"/><path d="M10 18v3M12 18v3M14 18v3" stroke="#6b7385" stroke-width="1.2" stroke-linecap="round"/>`,
    vip: `<path d="M3 8l4.5 4L12 5l4.5 7L21 8l-2 11H5z" fill="#f2b81c" stroke="#a87a08" stroke-width="1.1" stroke-linejoin="round"/><circle cx="3" cy="8" r="1.4" fill="#f2b81c"/><circle cx="12" cy="5" r="1.4" fill="#f2b81c"/><circle cx="21" cy="8" r="1.4" fill="#f2b81c"/>`,
    kingdom: `<path d="M3 21V9h3V6h2v3h2V6h2v3h2V6h2v3h3v12h-6v-5a3 3 0 0 0-6 0v5z" fill="#b7bdca" stroke="#5c6473" stroke-width="1" stroke-linejoin="round"/><path d="M12 5V1.5l4 1.3-4 1.2" fill="#d94b4b" stroke="#8f2323" stroke-width=".8"/>`,
    pin: `<path d="M12 22s7-6.4 7-12a7 7 0 0 0-14 0c0 5.6 7 12 7 12z" fill="#e2564b" stroke="#8f2323" stroke-width="1.1" stroke-linejoin="round"/><circle cx="12" cy="10" r="2.6" fill="#fff"/>`,
    march: `<path d="M5 22V3" stroke="#6b3f18" stroke-width="1.8" stroke-linecap="round"/><path d="M5 4h14l-3.5 4.5L19 13H5z" fill="#d94b4b" stroke="#8f2323" stroke-width="1" stroke-linejoin="round"/>`,
    alliance: `<circle cx="8.5" cy="8" r="3.2" fill="#8fa6d6" stroke="#3d5490" stroke-width="1"/><circle cx="16.5" cy="8" r="3.2" fill="#b4c4e6" stroke="#3d5490" stroke-width="1"/><path d="M2.5 20c0-3.6 2.7-5.8 6-5.8s6 2.2 6 5.8zM12 20c.4-2.4 2.2-4 4.6-4 3 0 5 2 5 4z" fill="#8fa6d6" stroke="#3d5490" stroke-width="1" stroke-linejoin="round"/>`,
    player: `<circle cx="12" cy="8.5" r="4.2" fill="#f0d3a8" stroke="#8a6a3a" stroke-width="1.1"/><path d="M4 21c0-4.6 3.6-7.4 8-7.4s8 2.8 8 7.4z" fill="#5b7fa8" stroke="#2e4a6b" stroke-width="1.1" stroke-linejoin="round"/><path d="M7.8 8c.6-3 2.4-4.5 4.4-4.5s3.8 1.5 4.2 4.5c-1.4-1.2-2.6-1.6-4.2-1.6S9.2 6.8 7.8 8z" fill="#7b4a22"/>`,
    infantry: `<path d="M18.5 2.5 21.5 5.5 11 16l-2-2z" fill="#dfe3ea" stroke="#5c6473" stroke-width="1" stroke-linejoin="round"/><path d="M7.5 12.5 11.5 16.5M6 18l-3 3M5 14.5l4.5 4.5" stroke="#7d4b22" stroke-width="2" stroke-linecap="round" fill="none"/>`,
    cavalry: `<path d="M5 20a8 8 0 1 1 14 0h-4a4 4 0 1 0-6 0z" fill="#c9ccd4" stroke="#5c6473" stroke-width="1.3" stroke-linejoin="round"/><path d="M5 20h4M15 20h4" stroke="#5c6473" stroke-width="2" stroke-linecap="round"/><circle cx="12" cy="5.5" r="1" fill="#5c6473"/>`,
    ranged: `<path d="M7 3c8 2 8 16 0 18" fill="none" stroke="#7d4b22" stroke-width="2" stroke-linecap="round"/><path d="M7 3v18" stroke="#c9ccd4" stroke-width="1"/><path d="M4 12h16m-4-3.2L20 12l-4 3.2" fill="none" stroke="#5c6473" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>`,
    siege: `<circle cx="7" cy="18" r="3.2" fill="#a86a32" stroke="#6b3f18" stroke-width="1"/><circle cx="17" cy="18" r="3.2" fill="#a86a32" stroke="#6b3f18" stroke-width="1"/><path d="M4 15h16v-3H4zM6 12 15 4l3 3" fill="#c98b4b" stroke="#6b3f18" stroke-width="1.1" stroke-linejoin="round"/><circle cx="18" cy="6.5" r="2.2" fill="#6b7385" stroke="#3a4152" stroke-width="1"/>`,
    total: `<path d="M12 2.5 15 9l7 .8-5.2 4.7 1.5 7L12 18l-6.3 3.5 1.5-7L2 9.8 9 9z" fill="#f2b81c" stroke="#a87a08" stroke-width="1" stroke-linejoin="round"/>`,
    wounded: `<rect x="3" y="3" width="18" height="18" rx="4" fill="#fff" stroke="#c0392b" stroke-width="1.4"/><path d="M12 6.5v11M6.5 12h11" stroke="#d94b4b" stroke-width="3.2" stroke-linecap="round"/>`,
    clock: `<circle cx="12" cy="12" r="9.5" fill="#eef0f5" stroke="#6b7385" stroke-width="1.2"/><path d="M12 6.5V12l3.6 2.2" fill="none" stroke="#3a4152" stroke-width="1.6" stroke-linecap="round"/>`,
  };
  const icon = (name, size = 22) => `<svg class="ico" width="${size}" height="${size}" viewBox="0 0 24 24" aria-hidden="true">${ICONS[name] || ""}</svg>`;

  function gameHtml(game) {
    if (!game) return `<div class="card"><p class="help">Chargement…</p></div>`;
    if (!game.available) {
      return `<div class="card"><p><strong>Aucune donnée pour l'instant.</strong></p>
        <p class="help">${game.running ? "Le bot démarre : les informations arrivent dès qu'il est en jeu."
          : "Démarrez le bot pour voir l'état du compte."}</p></div>`;
    }
    const d = game.data;
    const elapsed = game.fetched ? (Date.now() - game.fetched) / 1000 : 0;   // counts down between two polls
    const sh = d.shield;
    let shield;
    if (!sh.loaded) shield = `<span class="pill">inconnu</span>`;
    else if (!sh.active) shield = `<span class="pill bad">aucun bouclier</span>`;
    else {
      const left = Math.max(0, sh.remaining - (game.age + elapsed));
      shield = `<span class="pill ${left < 3600 ? "warn" : "ok"}" data-shield-left="${sh.remaining - game.age}">${fmtDuration(left)} restantes</span>
        <span class="help"> · bouclier ${SHIELDS[sh.item_id] || "#" + sh.item_id}</span>`;
    }
    const state = game.live ? `<span class="pill ok">En ligne</span>`
      : game.running ? `<span class="pill warn">Bot démarré, hors ligne (reconnexion…)</span>` : `<span class="pill">Bot arrêté</span>`;
    const stat = (ico, label, value) => `<div class="stat"><span class="badge">${icon(ico, 26)}</span>
      <span class="txt"><span class="k">${label}</span><span class="v">${value}</span></span></div>`;
    const tiles = RSS.map(([k, label]) => {
      const prod = d.production[k];
      return `<div class="rtile ${k}" title="${label} : ${num(d.resources[k])}">
        <div class="rtop">${icon(k, 30)}<span class="rname">${label}</span></div>
        <div class="rval">${short(d.resources[k])}</div>
        <div class="rsub">Sac : ${short(d.bag[k])}</div>
        <div class="rsub ${prod < 0 ? "neg" : "pos"}">${prod > 0 ? "+" : ""}${num(prod)}/h</div></div>`;
    }).join("");
    const t = d.troops;
    const troop = (ico, label, value) => `<div class="ttile">${icon(ico, 34)}<span class="tval">${value}</span><span class="tname">${label}</span></div>`;
    const home = d.home_kingdom && d.home_kingdom !== d.kingdom ? ` <span class="help">(origine ${d.home_kingdom})</span>` : "";
    return `
      <div class="hero">
        <div class="avatar">${icon("player", 44)}</div>
        <div class="who"><div class="pname">${esc(d.name || "—")}</div>
          <div class="psub">${icon("kingdom", 16)} Royaume ${d.kingdom}${home}
            <span class="sep">·</span>${icon("pin", 16)} ${d.x}, ${d.y}</div></div>
        <div class="hpower">${icon("power", 28)}<span class="k">Puissance</span><span class="v">${num(d.power)}</span></div>
        <div class="hstate">${state}</div>
      </div>
      <div class="card"><div class="statgrid">
        ${stat("shield", "Bouclier", shield)}
        ${stat("kills", "Kills", num(d.kills))}
        ${stat("gems", "Gemmes", num(d.gems))}
        ${stat("vip", "VIP", `${d.vip_level} <span class="help">(${num(d.vip_points)} pts)</span>`)}
        ${stat("march", "Marches", `${d.current_marches ?? d.marches} / ${d.max_marches}`)}
        ${stat("alliance", "Alliance", `${RANKS[d.alliance.rank] || d.alliance.rank}${d.alliance.members ? ` · ${d.alliance.members} membres` : ""}`)}
      </div>
      <p class="help">${icon("clock", 14)} Mis à jour il y a ${fmtDuration(game.age)}.</p></div>
      <div class="card"><h2>Ressources</h2><div class="rgrid">${tiles}</div></div>
      <div class="card"><h2>Troupes</h2>
        ${t.loaded ? `<div class="tgrid">
          ${troop("total", "Total", num(t.total))}${troop("infantry", "Infanterie", short(t.infantry))}${troop("cavalry", "Cavalerie", short(t.cavalry))}
          ${troop("ranged", "Tireurs", short(t.ranged))}${troop("siege", "Siège", short(t.siege))}
          ${d.wounded.loaded ? troop("wounded", "Blessés", num(d.wounded.total)) : ""}</div>`
          : `<p class="help">Pas encore reçues du serveur.</p>`}</div>`;
  }

  function startGame() {
    S.game = null;
    pollGame();
    // the shield countdown moves every second without asking the server again
    S.gameTick = setInterval(() => {
      const box = $("#game");
      if (box && S.game && S.game.available && S.game.data.shield.active) box.innerHTML = gameHtml(S.game);
    }, 1000);
  }

  async function pollGame() {
    clearTimeout(S.gameTimer);
    if (S.view !== "account" || (S.tab !== "status" && S.tab !== "chat") || !S.acc) return;
    const id = S.id;
    try {
      const game = await api("GET", `/api/accounts/${enc(id)}/game`);
      if (id === S.id && S.view === "account" && (S.tab === "status" || S.tab === "chat")) {
        game.fetched = Date.now();
        S.game = game;
        const box = $("#game");
        if (box) box.innerHTML = gameHtml(game);
        // the message list only: the input/form must survive the poll untouched (focus, draft text)
        const log = $("#chat-log");
        if (log) { const stick = log.scrollTop + log.clientHeight >= log.scrollHeight - 24;
          log.innerHTML = chatLogHtml(game); if (stick) log.scrollTop = log.scrollHeight; }
      }
    } catch (error) {
      if (error.status === 401) return authProblem();
    }
    S.gameTimer = setTimeout(pollGame, 4000);
  }

  // ------------------------------------------------------------ guild chat

  function chatLogHtml(game) {
    if (!game) return `<p class="help">Chargement…</p>`;
    if (!game.available) {
      return `<p class="help">${game.running ? "Le bot démarre : le chat arrive dès qu'il est en jeu."
        : "Démarrez le bot pour voir le chat de guilde."}</p>`;
    }
    const msgs = game.data.guild_chat;
    if (!msgs || !msgs.length) return `<p class="help">Aucun message récent.</p>`;
    return msgs.map((m) => {
      const time = new Date(m.time * 1000).toLocaleTimeString("fr-FR", { hour: "2-digit", minute: "2-digit" });
      return `<div class="chat-line"><span class="chat-time">${time}</span>
        <span class="chat-player">${esc(m.player)}</span><span class="chat-text">${esc(m.message)}</span></div>`;
    }).join("");
  }

  function chatView() {
    return `<div class="card chat-card">
      <div class="chat-log" id="chat-log">${chatLogHtml(S.game)}</div>
      <div class="inline chat-form">
        <input type="text" id="chat-input" maxlength="240" placeholder="Écrire dans le chat de guilde…" autocomplete="off">
        <button class="btn primary" data-act="chat-send">Envoyer</button>
      </div>
    </div>`;
  }

  async function chatSend() {
    const input = $("#chat-input");
    if (!input) return;
    const message = input.value.trim();
    if (!message) return;
    input.disabled = true;
    try {
      await api("POST", `/api/accounts/${enc(S.id)}/chat/send`, { message });
      input.value = "";
    } catch (error) {
      toast(error.message, "err");
    } finally {
      input.disabled = false;
      input.focus();
    }
  }

  // ------------------------------------------------------------ guild bank

  const BANK_RES = ["food", "stone", "wood", "ore", "gold"];

  function bankView() {
    return `<div class="card bank-card">
      <div class="bank-tools">
        <input type="search" id="bank-filter" placeholder="Filtrer les joueurs…" autocomplete="off" value="${esc(S.bankFilter || "")}">
        <button class="btn danger" data-act="bank-reset">Tout remettre à zéro</button>
      </div>
      <div id="bank-body">${bankBodyHtml(S.bank)}</div>
    </div>`;
  }

  function bankBodyHtml(bank) {
    if (!bank) return `<p class="help">Chargement…</p>`;
    const notes = [];
    if (!bank.enabled) {
      notes.push(`<div class="note warn">La banque de guilde n'est pas activée pour ce compte (réglage « Banque de guilde »). Les soldes s'affichent, mais le bot ne les utilise pas et, tant qu'il tourne, ignorerait une modification : activez-la puis redémarrez le bot.</div>`);
    }
    if (!bank.members_known) {
      notes.push(`<div class="note">La liste des membres de la guilde n'est pas encore connue (le bot ne l'a pas reçue) : seuls les joueurs qui ont un solde sont listés.</div>`);
    }
    const where = bank.storage === "sqlserver" ? "base de données SQL Server" : "fichiers (data/…/guild_bank.txt)";
    const how = bank.running
      ? "Le bot tourne : il applique vos modifications dans la seconde qui suit."
      : "Le bot est arrêté : les modifications sont enregistrées et prises en compte à son démarrage.";
    const filter = (S.bankFilter || "").trim().toLowerCase();
    const players = bank.players.filter((p) => !filter || p.name.toLowerCase().includes(filter));
    const heads = bank.resources.map((r) => `<th class="num">${esc(r.label)}</th>`).join("");
    const rows = players.map((p) => {
      const out = p.in_guild ? "" : ` <span class="badge-out" title="Ce joueur a un solde mais n'est plus dans la guilde du bot">hors guilde</span>`;
      if (S.bankEdit === p.name) {
        const inputs = BANK_RES.map((k) => `<td class="num"><input class="bank-input" type="text" inputmode="numeric" data-res="${k}" value="${p[k]}" aria-label="${esc(p.name)} : ${k}"></td>`).join("");
        return `<tr class="editing"><td class="bank-name">${esc(p.name)}${out}</td>${inputs}
          <td class="bank-act"><button class="btn primary small" data-act="bank-save">Enregistrer</button> <button class="btn small" data-act="bank-cancel">Annuler</button></td></tr>`;
      }
      const cells = BANK_RES.map((k) => `<td class="num${p[k] ? "" : " zero"}">${num(p[k])}</td>`).join("");
      return `<tr><td class="bank-name">${esc(p.name)}${out}</td>${cells}
        <td class="bank-act"><button class="btn small" data-act="bank-edit" data-name="${esc(p.name)}">Modifier</button></td></tr>`;
    }).join("");
    const totals = BANK_RES.map((k) => `<td class="num">${num(bank.totals[k] || 0)}</td>`).join("");
    const table = players.length
      ? `<div class="bank-scroll"><table class="bank-table">
          <thead><tr><th>Joueur</th>${heads}<th></th></tr></thead>
          <tbody>${rows}</tbody>
          <tfoot><tr><td>Total (${bank.players.length} joueur${bank.players.length > 1 ? "s" : ""})</td>${totals}<td></td></tr></tfoot>
        </table></div>`
      : `<p class="help">${bank.players.length ? "Aucun joueur ne correspond au filtre." : "Aucun joueur pour l'instant : les membres apparaissent dès que le bot connaît la guilde, et les soldes dès le premier dépôt."}</p>`;
    return `${notes.join("")}<p class="help">Stockage : ${where}. ${how} Un dépôt se fait en envoyant des ressources au bot ; le solde est le montant net reçu.</p>${table}`;
  }

  function renderBank() {
    const body = $("#bank-body");
    if (body) body.innerHTML = bankBodyHtml(S.bank);
  }

  function startBank() {
    S.bank = null;
    S.bankEdit = null;
    const filter = $("#bank-filter");
    if (filter) filter.addEventListener("input", () => { S.bankFilter = filter.value; if (!S.bankEdit) renderBank(); });
    const body = $("#bank-body");
    if (body) body.addEventListener("keydown", (event) => {
      if (event.key === "Enter" && event.target.classList.contains("bank-input")) { event.preventDefault(); bankSave(); }
      if (event.key === "Escape" && S.bankEdit) { S.bankEdit = null; renderBank(); }
    });
    pollBank();
  }

  async function pollBank() {
    clearTimeout(S.bankTimer);
    if (S.view !== "account" || S.tab !== "bank" || !S.acc) return;
    const id = S.id;
    try {
      const bank = await api("GET", `/api/accounts/${enc(id)}/bank`);
      if (id === S.id && S.view === "account" && S.tab === "bank") {
        S.bank = bank;
        if (!S.bankEdit) renderBank();   // never under the fingers of somebody typing an amount
      }
    } catch (error) {
      if (error.status === 401) return authProblem();
    }
    S.bankTimer = setTimeout(pollBank, 5000);
  }

  // "5M", "1,5m", "500 000", "2b" -> a whole number, or null
  function parseAmount(text) {
    const t = String(text).trim().toLowerCase().replace(/[\s\u00a0\u202f_]/g, "").replace(",", ".");
    if (t === "") return 0;
    const m = /^(\d+(?:\.\d+)?)([kmb]?)$/.exec(t);
    if (!m) return null;
    const value = Math.round(parseFloat(m[1]) * { "": 1, k: 1e3, m: 1e6, b: 1e9 }[m[2]]);
    return value <= 1e15 ? value : null;
  }

  async function bankSave() {
    const name = S.bankEdit;
    const player = S.bank && S.bank.players.find((p) => p.name === name);
    if (!player) return;
    const changes = {};
    for (const input of $$(".bank-input")) {
      const amount = parseAmount(input.value);
      if (amount === null) { toast(`Montant invalide : « ${input.value} » (exemples : 500000, 2,5M, 1B).`, "err"); input.focus(); return; }
      if (amount !== player[input.dataset.res]) changes[input.dataset.res] = amount;
    }
    if (!Object.keys(changes).length) { S.bankEdit = null; renderBank(); return; }
    const id = S.id;
    try {
      const result = await api("PUT", `/api/accounts/${enc(id)}/bank/${enc(name)}`, changes);
      if (id !== S.id) return;
      S.bank = result;
      S.bankEdit = null;
      renderBank();
      toast(result.pending ? "Modification envoyée : le bot ne l'a pas encore appliquée, elle le sera dès qu'il le pourra." : "Solde modifié", result.pending ? "" : "ok");
    } catch (error) {
      toast(error.message, "err");
    }
  }

  async function bankReset() {
    const bank = S.bank;
    const holders = bank ? bank.players.filter((p) => BANK_RES.some((k) => p[k] > 0)).length : 0;
    if (!confirm(`Remettre à zéro TOUS les soldes de la banque de guilde ?\n\n${holders} joueur${holders > 1 ? "s ont" : " a"} un solde. `
        + "Ils perdent ce qu'ils avaient déposé : cette action est définitive."
        + (bank && bank.running ? "" : "\n\n(Le bot est arrêté : elle sera prise en compte à son démarrage.)"))) return;
    const id = S.id;
    try {
      const result = await api("POST", `/api/accounts/${enc(id)}/bank/reset`, { confirm: true });
      if (id !== S.id) return;
      S.bank = result;
      S.bankEdit = null;
      renderBank();
      toast(result.pending ? "Remise à zéro envoyée : le bot ne l'a pas encore appliquée." : "Tous les soldes sont remis à zéro", result.pending ? "" : "ok");
    } catch (error) {
      toast(error.message, "err");
    }
  }

  // --------------------------------------------------------------- actions

  async function refreshState() {
    try {
      const state = await api("GET", "/api/state");
      const changed = JSON.stringify(state.accounts) !== JSON.stringify(S.accounts);
      S.accounts = state.accounts;
      S.settings = state.settings;
      S.schema = state.schema;
      S.os = state.os;
      if (S.view === "account" && S.acc) {
        const summary = accountById(S.id);
        if (summary) S.acc.status = summary.status;
        refreshControls();
      }
      if (changed) renderSidebar();
    } catch (error) {
      if (error.status === 401) authProblem();
    }
  }

  async function openAccount(id, tab) {
    if (!confirmDiscard()) return;
    const ticket = ++S.nav;
    try {
      const payload = await api("GET", `/api/accounts/${enc(id)}`);
      if (ticket !== S.nav) return;   // you went somewhere else while it loaded
      S.view = "account";
      S.id = id;
      S.acc = payload;
      S.form = initialForm(payload);
      S.errors = {};
      S.prefix = payload.values["command.prefix"] || "$";
      if (tab) S.tab = tab;
      // an account without identifiers has nothing to show yet: start where they are entered
      if (tab && !payload.values["account.igg_id"]) S.tab = "technical";
      try { sessionStorage.setItem("lmbot-last", id); } catch (_) { /* optional */ }
      render();
    } catch (error) {
      toast(error.message, "err");
    }
  }

  function showView(view) {
    if (!confirmDiscard()) return;
    S.nav++;
    S.view = view;
    S.acc = view === "account" ? S.acc : null;
    S.form = {};
    S.errors = {};
    if (view === "add") S.importResult = null;
    render();
    if (view === "add") loadCapture();
  }

  async function save(restart) {
    const keys = dirtyKeys();
    if (!keys.length && !restart) return;
    const changes = {};
    keys.forEach((key) => { changes[key] = S.form[key]; });
    try {
      if (keys.length) {
        S.acc = await api("PUT", `/api/accounts/${enc(S.id)}`, { changes });
        S.form = initialForm(S.acc);
        S.errors = {};
        if (S.acc.status.state === "running") S.pendingRestart = S.id;
        toast("Réglages enregistrés", "ok");
      }
      if (restart) {
        S.acc.status = await api("POST", `/api/accounts/${enc(S.id)}/restart`);
        S.pendingRestart = null;
        toast("Bot redémarré", "ok");
      }
    } catch (error) {
      if (error.data && error.data.errors) {
        S.errors = error.data.errors;
        const first = Object.keys(S.errors)[0];
        const category = S.schema.categories.find((c) => c.fields.some((f) => f.key === first));
        if (category) S.tab = category.technical ? "technical" : category.id;
        toast("Certains réglages sont invalides : corrigez-les puis réessayez.", "err");
      } else {
        toast(error.message, "err");
      }
    }
    await refreshState();
    render();
    const firstError = $(".error:not([hidden])");
    if (firstError) firstError.scrollIntoView({ block: "center" });
  }

  async function control(action) {
    try {
      S.acc.status = await api("POST", `/api/accounts/${enc(S.id)}/${action}`);
      if (action !== "start") S.pendingRestart = null;
      if (action === "stop") toast("Bot arrêté");
      if (action === "start") toast("Bot démarré", "ok");
      if (action === "restart") toast("Bot redémarré", "ok");
    } catch (error) {
      toast(error.message, "err");
    }
    await refreshState();
    refreshControls();
    if (S.tab === "logs") { S.logOffset = -1; pollLogs(); }
  }

  async function startAll() {
    const stagger = Number(S.settings.stagger) || 0;
    const todo = scopedAccounts().filter((a) => a.status.state !== "running" && a.has_key && a.igg_id);
    for (let i = 0; i < todo.length; i++) {
      try {
        await api("POST", `/api/accounts/${enc(todo[i].id)}/start`);
        toast(`${displayName(todo[i])} démarré`, "ok");
      } catch (error) {
        toast(`${displayName(todo[i])} : ${error.message}`, "err");
      }
      await refreshState();
      if (i < todo.length - 1 && stagger) await new Promise((resolve) => setTimeout(resolve, stagger * 1000));
    }
    if (!todo.length) toast("Aucun compte à démarrer.");
  }

  async function stopAll() {
    const todo = scopedAccounts().filter((a) => a.status.state === "running");
    for (const account of todo) {
      try { await api("POST", `/api/accounts/${enc(account.id)}/stop`); } catch (error) { toast(error.message, "err"); }
    }
    toast(S.tagFilter ? `Comptes "${S.tagFilter}" arrêtés` : "Tous les bots sont arrêtés");
    await refreshState();
  }

  async function importFile(file) {
    if (!file) return;
    S.importing = true;
    S.importResult = null;
    renderMain();
    try {
      const result = await api("POST", "/api/import", file);
      S.importResult = result.accounts;
      toast(`${result.accounts.length} compte(s) importé(s)`, "ok");
    } catch (error) {
      toast(error.message, "err");
    }
    S.importing = false;
    await refreshState();
    renderSidebar();
    renderMain();
  }

  async function createEmpty() {
    S.nav++;
    const name = $("#new-name").value;
    const copy = $("#new-copy") ? $("#new-copy").value : "";
    try {
      const payload = await api("POST", "/api/accounts", { name, copy_from: copy || null });
      await refreshState();
      S.view = "account";
      S.id = payload.id;
      S.acc = payload;
      S.form = initialForm(payload);
      S.errors = {};
      S.tab = "technical";
      render();
      toast("Compte créé. Renseignez ses identifiants.", "ok");
    } catch (error) {
      toast(error.message, "err");
    }
  }

  async function saveSettings() {
    ["client", "stagger", "remote_url"].forEach((name) => { const el = $(`#set-${name}-err`); el.hidden = true; });
    try {
      S.settings = await api("PUT", "/api/settings", {
        client_path: $("#set-client").value,
        stagger: $("#set-stagger").value,
        remote_url: $("#set-remote_url").value,
        remote_token: $("#set-remote_token").value,
      });
      toast("Paramètres enregistrés", "ok");
      renderMain();
    } catch (error) {
      const errors = (error.data && error.data.errors) || {};
      Object.entries(errors).forEach(([key, message]) => {
        const el = $(`#set-${key === "client_path" ? "client" : key}-err`);
        if (el) { el.textContent = message; el.hidden = false; }
      });
      toast(error.message, "err");
    }
  }

  function showTransfer(result, where) {
    const done = result.imported.length, skipped = result.skipped || [];
    toast(`${done} compte${done > 1 ? "s" : ""} ${where}` + (skipped.length ? `, ${skipped.length} ignoré${skipped.length > 1 ? "s" : ""} (${skipped.map((x) => x.id + " : " + x.reason).join(" ; ")})` : ""),
      skipped.length && !done ? "err" : "ok");
  }

  async function dataExport() {
    try {
      const bundle = await api("GET", "/api/data/export");
      const url = URL.createObjectURL(new Blob([JSON.stringify(bundle, null, 2)], { type: "application/json" }));
      const link = Object.assign(document.createElement("a"), { href: url, download: `lmbot-sauvegarde-${new Date().toISOString().slice(0, 10)}.json` });
      document.body.appendChild(link); link.click(); link.remove();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
      toast("Sauvegarde téléchargée : elle contient vos clés d'accès", "ok");
    } catch (error) { toast(error.message, "err"); }
  }

  async function dataImport(file) {
    if (!file) return;
    try {
      const bundle = JSON.parse(await file.text());
      const overwrite = confirm("Remplacer les comptes qui existent déjà ?\nOK = remplacer, Annuler = les conserver et n'ajouter que les nouveaux.");
      const result = await api("POST", `/api/data/import?overwrite=${overwrite ? 1 : 0}`, bundle);
      showTransfer(result, "restauré(s)");
      await refreshState();
    } catch (error) { toast(error instanceof SyntaxError ? "Ce fichier n'est pas un JSON valide." : error.message, "err"); }
  }

  async function dataPush() {
    const overwrite = confirm("Envoyer tous les comptes de cet ordinateur au serveur.\nOK = remplacer ceux qui existent déjà là-bas, Annuler = ne garder que les nouveaux.");
    try {
      showTransfer(await api("POST", `/api/data/push?overwrite=${overwrite ? 1 : 0}`), "envoyé(s) au serveur");
    } catch (error) { toast(error.message, "err"); }
  }

  function authProblem() {
    stopLogs();
    $("#app").innerHTML = `<main style="margin:10vh auto;max-width:520px;padding:24px"><div class="notice err">
      <p><strong>Accès refusé (401).</strong></p>
      <p>La requête a été bloquée par quelque chose entre le navigateur et la console (proxy, extension...) :
      la console elle-même ne demande plus de jeton d'accès. Rechargez la page.</p></div></main>`;
  }

  const ACTIONS = {
    "open-account": (el) => openAccount(el.dataset.id, S.view === "account" ? undefined : "status"),
    "add-view": () => showView("add"),
    "settings-view": () => showView("settings"),
    "help-view": () => showView("help"),
    tab: (el) => { S.tab = el.dataset.tab; renderMain(); },
    start: () => control("start"),
    stop: () => control("stop"),
    restart: () => control("restart"),
    save: () => save(false),
    "save-restart": () => save(true),
    discard: () => { S.form = initialForm(S.acc); S.errors = {}; renderMain(); },
    rename: () => startRename(),
    "rename-account": async (el) => {
      if (S.view === "account" && S.id === el.dataset.id) { startRename(); return; }
      await openAccount(el.dataset.id, "status");
      if (S.view === "account" && S.id === el.dataset.id) startRename();
    },
    "rename-save": () => saveRename(),
    "rename-cancel": () => cancelRename(),
    "rename-chip": (el) => { const input = $("#rename-input"); if (input) { input.value = el.dataset.name; input.focus(); } },
    "tag-chip": (el) => { const input = $("#tag-input"); if (input) { input.value = el.dataset.name; input.focus(); } },
    "capture-start": () => captureStart(),
    "capture-stop": () => captureStop(),
    "capture-cancel": () => captureCancel(),
    "chat-send": () => chatSend(),
    "bank-edit": (el) => { S.bankEdit = el.dataset.name; renderBank(); const input = $(".bank-input"); if (input) { input.focus(); input.select(); } },
    "bank-save": () => bankSave(),
    "bank-cancel": () => { S.bankEdit = null; renderBank(); },
    "bank-reset": () => bankReset(),
    "delete-account": async () => {
      if (!confirm(`Supprimer définitivement le compte « ${S.id} » et ses identifiants ?`)) return;
      const ticket = ++S.nav;   // we are leaving this account: anything slower than the user must not navigate for them
      try {
        await api("DELETE", `/api/accounts/${enc(S.id)}`);
        toast("Compte supprimé");
        if (ticket === S.nav) { S.acc = null; S.form = {}; S.view = "welcome"; }
        await refreshState();
        if (ticket !== S.nav) { renderSidebar(); return; }   // the user already went somewhere else
        if (S.accounts.length) await openAccount(S.accounts[0].id, "status"); else render();
      } catch (error) { toast(error.message, "err"); }
    },
    "create-empty": createEmpty,
    "start-all": startAll,
    "stop-all": stopAll,
    "save-settings": saveSettings,
    "data-export": dataExport,
    "data-push": dataPush,
    "use-client": (el) => { $("#set-client").value = el.dataset.path; },
    "clear-log": () => { const box = $("#log"); if (box) box.textContent = ""; },
    copy: async (el) => {
      try { await navigator.clipboard.writeText(el.dataset.text); toast("Copié", "ok"); }
      catch (_) { toast("Copie impossible : sélectionnez le texte à la main.", "err"); }
    },
    "shield-up": (el) => moveShield(el.dataset.key, +el.dataset.i, -1),
    "shield-down": (el) => moveShield(el.dataset.key, +el.dataset.i, +1),
    "shield-del": (el) => {
      const list = splitShields(S.form[el.dataset.key]);
      list.splice(+el.dataset.i, 1);
      setShields(el.dataset.key, list);
    },
  };

  function moveShield(key, index, delta) {
    const list = splitShields(S.form[key]);
    const target = index + delta;
    if (target < 0 || target >= list.length) return;
    [list[index], list[target]] = [list[target], list[index]];
    setShields(key, list);
  }

  function setShields(key, list) {
    S.form[key] = list.join(", ");
    delete S.errors[key];
    const field = S.schema.categories.flatMap((c) => c.fields).find((f) => f.key === key);
    const holder = $(`[data-shields="${key}"]`);
    if (holder && field) holder.outerHTML = shieldsHtml(field);
    applyDerived();
    refreshTabCounts();
    renderSavebar();
  }

  // ---------------------------------------------------------------- events

  function onClick(event) {
    const el = event.target.closest("[data-act]");
    if (!el || el.disabled) return;
    const handler = ACTIONS[el.dataset.act];
    if (handler) { event.preventDefault(); handler(el); }
  }

  function onInput(event) {
    const el = event.target;
    if (el.dataset.shieldAdd !== undefined) {
      if (!el.value) return;
      const key = el.dataset.shieldAdd;
      setShields(key, [...splitShields(S.form[key]), el.value]);
      return;
    }
    if (el.dataset.channelKey !== undefined) {
      const channelKey = el.dataset.channelKey;
      const field = S.schema.categories.flatMap((c) => c.fields).find((f) => f.key === channelKey);
      const checked = $$(`[data-channel-key="${channelKey}"]`).filter((box) => box.checked).map((box) => box.dataset.channel);
      if (!checked.length) { el.checked = true; toast("Gardez au moins un canal.", ""); return; }
      S.form[channelKey] = field.options.map(([value]) => value).filter((value) => checked.includes(value)).join(", ");
      delete S.errors[channelKey];
      refreshTabCounts();
      renderSavebar();
      return;
    }
    const key = el.dataset.key;
    if (!key || !S.acc) return;
    S.form[key] = el.type === "checkbox" ? String(el.checked) : el.value;
    if (S.errors[key]) {
      delete S.errors[key];
      el.classList.remove("invalid");
      const message = $(`[data-err="${key}"]`);
      if (message) message.hidden = true;
    }
    applyDerived();
    refreshTabCounts();
    renderSavebar();
  }

  function bindDrop() {
    document.addEventListener("dragover", (event) => {
      const drop = $("#drop");
      if (drop && drop.contains(event.target)) { event.preventDefault(); drop.classList.add("over"); }
    });
    document.addEventListener("dragleave", () => { const drop = $("#drop"); if (drop) drop.classList.remove("over"); });
    document.addEventListener("drop", (event) => {
      const drop = $("#drop");
      if (drop && drop.contains(event.target)) {
        event.preventDefault();
        drop.classList.remove("over");
        importFile(event.dataTransfer.files[0]);
      }
    });
  }

  // ---------------------------------------------------------------- rename

  const NAME_SUGGESTIONS = ["Bank", "Filler", "Farm", "Main"];
  const TAG_SUGGESTIONS = ["Bank", "Filler", "Prison", "War", "Farm"];

  function startRename() {
    const title = $("#acc-title");
    if (!title || $("#rename-box")) return;
    const summary = accountById(S.id) || {};
    const box = document.createElement("div");
    box.id = "rename-box";
    box.className = "rename";
    box.innerHTML = `<div class="inline">
        <input type="text" id="rename-input" maxlength="40" value="${esc(summary.alias || "")}" placeholder="${esc(summary.igg_id ? "Compte " + summary.igg_id : S.id)}" aria-label="Nom du compte" spellcheck="false">
        <button class="btn primary small" data-act="rename-save">Enregistrer</button>
        <button class="btn small" data-act="rename-cancel">Annuler</button></div>
      <div class="chips"><span class="help">Suggestions :</span>${NAME_SUGGESTIONS.map((n) =>
        `<button class="chip-btn" data-act="rename-chip" data-name="${n}">${n}</button>`).join("")}
        <span class="help">ou tapez ce que vous voulez. Vide = nom par défaut.</span></div>
      <div class="inline" style="margin-top:10px">
        <label class="lbl" for="tag-input" style="margin:0">Tag (utilité du compte)</label>
        <input type="text" id="tag-input" maxlength="20" value="${esc(summary.tag || "")}" placeholder="ex. Bank, Filler, Prison…" aria-label="Tag du compte" spellcheck="false"></div>
      <div class="chips"><span class="help">Suggestions :</span>${TAG_SUGGESTIONS.map((n) =>
        `<button class="chip-btn" data-act="tag-chip" data-name="${n}">${n}</button>`).join("")}
        <span class="help">ou tapez ce que vous voulez. Vide = pas de tag.</span></div>`;
    title.hidden = true;
    title.after(box);
    const input = $("#rename-input");
    input.focus();
    input.select();
  }

  function cancelRename() {
    const box = $("#rename-box");
    if (box) box.remove();
    const title = $("#acc-title");
    if (title) title.hidden = false;
  }

  async function saveRename() {
    const input = $("#rename-input");
    const tagInput = $("#tag-input");
    if (!input) return;
    try {
      await api("PUT", `/api/accounts/${enc(S.id)}/alias`, { alias: input.value });
      if (tagInput) await api("PUT", `/api/accounts/${enc(S.id)}/tag`, { tag: tagInput.value });
      await refreshState();
      render();
      toast("Compte renommé", "ok");
    } catch (error) {
      toast(error.message, "err");
    }
  }

  // --------------------------------------------------------------- capture

  async function loadCapture() {
    try { S.capture = await api("GET", "/api/capture"); } catch (error) {
      if (error.status === 401) return authProblem();
    }
    refreshCaptureCard();
    pollCapture();
  }

  function refreshCaptureCard() {
    const card = $("#capture-card");
    if (card) card.outerHTML = captureCard();
  }

  function pollCapture() {
    clearTimeout(S.capPoll);
    if (S.view !== "add" || !S.capture || !["starting", "recording"].includes(S.capture.state)) return;
    S.capPoll = setTimeout(async () => {
      try {
        const next = await api("GET", "/api/capture");
        const changed = next.state !== S.capture.state;
        S.capture = next;
        if (changed) refreshCaptureCard();
      } catch (_) { /* try again on the next tick */ }
      pollCapture();
    }, 1000);
  }

  async function captureStart() {
    const all = $("#cap-all") && $("#cap-all").checked;
    try {
      S.capture = { ...(await api("POST", "/api/capture/start", { all_tcp: !!all })), available: true };
    } catch (error) {
      S.capture = { state: "error", available: S.capture ? S.capture.available : true, message: error.message };
    }
    refreshCaptureCard();
    pollCapture();
  }

  async function captureStop() {
    S.capture = { ...S.capture, state: "processing" };
    refreshCaptureCard();
    try {
      const result = await api("POST", "/api/capture/stop");
      S.importResult = result.accounts;
      S.capture = { state: "idle", available: true };
      toast(`${result.accounts.length} compte(s) importé(s)`, "ok");
      await refreshState();
    } catch (error) {
      S.capture = { state: "error", available: true, message: error.message };
      toast(error.message, "err");
    }
    renderSidebar();
    if (S.view === "add") renderMain();
  }

  async function captureCancel() {
    try { await api("POST", "/api/capture/cancel"); } catch (_) { /* nothing to cancel */ }
    S.capture = { state: "idle", available: true };
    refreshCaptureCard();
  }

  // ------------------------------------------------------------------ boot

  async function boot() {
    renderShell();
    document.addEventListener("click", onClick);
    document.addEventListener("input", onInput);
    document.addEventListener("change", (event) => {
      if (event.target.id === "file") importFile(event.target.files[0]);
      if (event.target.id === "data-file") { dataImport(event.target.files[0]); event.target.value = ""; }
      if (event.target.id === "tag-filter") { S.tagFilter = event.target.value; renderSidebar(); }
    });
    document.addEventListener("keydown", (event) => {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "s" && S.view === "account" && dirtyKeys().length) {
        event.preventDefault();
        save(false);
      }
    });
    document.addEventListener("keydown", (event) => {
      if (event.target && event.target.id === "rename-input") {
        if (event.key === "Enter") { event.preventDefault(); saveRename(); }
        if (event.key === "Escape") { event.preventDefault(); cancelRename(); }
      }
      if (event.target && event.target.id === "chat-input" && event.key === "Enter") {
        event.preventDefault();
        chatSend();
      }
    });
    window.addEventListener("beforeunload", (event) => { if (dirtyKeys().length) { event.preventDefault(); event.returnValue = ""; } });
    bindDrop();
    try {
      await refreshState();
    } catch (_) { return; }
    if (!S.schema) return;
    let last = null;
    try { last = sessionStorage.getItem("lmbot-last"); } catch (_) { /* optional */ }
    const first = accountById(last) || S.accounts[0];
    if (first) await openAccount(first.id, "status"); else render();
    setInterval(() => { if (!document.hidden) refreshState(); }, 3000);
  }

  boot();
})();
