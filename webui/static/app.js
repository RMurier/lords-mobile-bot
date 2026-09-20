"use strict";

(() => {
  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
  const enc = encodeURIComponent;
  const esc = (value) => String(value ?? "").replace(/[&<>"']/g,
    (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));

  const S = {
    token: null,
    schema: null,
    accounts: [],
    settings: {},
    os: "posix",
    view: "welcome",     // welcome | account | add | settings | help
    prefix: "$",         // command prefix of the last opened account, for the commands page
    id: null,
    tab: "account",
    acc: null,           // last payload of the open account
    form: {},            // current, possibly unsaved, values
    errors: {},
    pendingRestart: null,
    logOffset: -1,
    logPartial: "",
    logTimer: null,
    importResult: null,
    importing: false,
  };

  // ------------------------------------------------------------------ api

  async function api(method, path, body) {
    const options = { method, headers: { "X-Token": S.token || "" } };
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
    $("#toasts").appendChild(el);
    setTimeout(() => el.remove(), kind === "err" ? 7000 : 3500);
  }

  // -------------------------------------------------------------- helpers

  const accountById = (id) => S.accounts.find((a) => a.id === id);

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
    [/server error code|rejected by server|Login failed/i,
      "Le serveur a refusé les identifiants : la clé d'accès est probablement expirée. Réimportez une capture "
      + "(Ajouter un compte → Importer une capture)."],
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

  function renderSidebar() {
    const items = S.accounts.map((a) => {
      const kind = statusKind(a.status);
      const current = S.view === "account" && S.id === a.id;
      const sub = a.igg_id ? `IGG ${a.igg_id}` : (a.has_key ? a.id : "identifiants manquants");
      return `<button class="acc" data-act="open-account" data-id="${esc(a.id)}" ${current ? 'aria-current="true"' : ""}>
        <span class="dot ${kind}" title="${esc(statusText(a.status))}"></span>
        <span class="txt"><div class="name">${esc(displayName(a))}</div><div class="sub">${esc(sub)}</div></span>
      </button>`;
    }).join("");
    const running = S.accounts.filter((a) => a.status.state === "running").length;
    const multi = S.accounts.length > 1;
    $("#sidebar").innerHTML = `
      <div class="side-title">Comptes</div>
      ${items || '<p class="help" style="padding:0 8px">Aucun compte pour l\'instant.</p>'}
      <div class="side-actions"><button class="btn wide" data-act="add-view">＋ Ajouter un compte</button></div>
      ${multi ? `<div class="side-foot">
        <button class="btn wide" data-act="start-all">Tout démarrer</button>
        <button class="btn wide" data-act="stop-all" ${running ? "" : "disabled"}>Tout arrêter</button>
      </div>` : ""}`;
  }

  function renderMain() {
    stopLogs();
    const main = $("#main");
    if (S.view === "account" && S.acc) {
      main.innerHTML = accountView();
      applyDerived();
      if (S.tab === "logs") startLogs();
    } else if (S.view === "add") {
      main.innerHTML = addView();
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
    const tabs = S.schema.categories.map((c) => {
      const keys = c.fields.map((f) => f.key);
      const dirty = keys.filter((k) => S.form[k] !== a.values[k]).length;
      const bad = keys.filter((k) => S.errors[k]).length;
      const badge = bad ? `<span class="count bad">${bad}</span>` : (dirty ? `<span class="count">${dirty}</span>` : "");
      return `<button class="tab" role="tab" data-act="tab" data-tab="${c.id}" aria-selected="${S.tab === c.id}">${esc(c.label)}${badge}</button>`;
    }).join("");
    const logsTab = `<button class="tab" role="tab" data-act="tab" data-tab="logs" aria-selected="${S.tab === "logs"}">Journal</button>`;
    const category = S.schema.categories.find((c) => c.id === S.tab);
    return `
      <div class="head">
        <div class="title">
          <h1>${esc(displayName(summary))} <button class="iconbtn" data-act="alias" title="Renommer" aria-label="Renommer le compte">✎</button></h1>
          <div class="subtitle mono">accounts/${esc(a.id)}.cfg</div>
        </div>
        <div class="actions" id="ctrl">${ctrlHtml()}</div>
      </div>
      <div id="notes">${notesHtml()}</div>
      <div class="tabs" role="tablist">${tabs}${logsTab}</div>
      ${S.tab === "logs" ? logsView() : categoryView(category)}`;
  }

  function ctrlHtml() {
    const status = S.acc.status;
    const kind = statusKind(status);
    const chip = `<span class="chip ${kind}"><span class="dot ${kind}"></span>${esc(statusText(status))}</span>`;
    if (status.state === "running") {
      return `${chip}<button class="btn" data-act="restart">Redémarrer</button><button class="btn danger" data-act="stop">Arrêter</button>`;
    }
    return `${chip}<button class="btn ok" data-act="start">Démarrer</button>`;
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
    if (category.id === "account") html += dangerHtml();
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
      <p class="help">Supprime le fichier de configuration de ce compte (les identifiants seront perdus).</p>
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
    } else if (f.type === "shields") {
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
    const label = (name) => (S.schema.shields.find(([n]) => n === name) || [name, name])[1];
    const rows = current.map((name, i) => `<div class="shield-row">
      <span class="n">${i + 1}.</span><span class="l">${esc(label(name))}</span>
      <button class="iconbtn" data-act="shield-up" data-key="${esc(key)}" data-i="${i}" ${i === 0 ? "disabled" : ""} aria-label="Monter">↑</button>
      <button class="iconbtn" data-act="shield-down" data-key="${esc(key)}" data-i="${i}" ${i === current.length - 1 ? "disabled" : ""} aria-label="Descendre">↓</button>
      <button class="iconbtn" data-act="shield-del" data-key="${esc(key)}" data-i="${i}" aria-label="Retirer">✕</button></div>`).join("");
    const rest = S.schema.shields.filter(([name]) => !current.includes(name));
    const add = rest.length ? `<select data-shield-add="${esc(key)}" aria-label="Ajouter un bouclier">
      <option value="">Ajouter un bouclier…</option>${rest.map(([name, text]) => `<option value="${esc(name)}">${esc(text)}</option>`).join("")}</select>` : "";
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

  function addView() {
    const results = (S.importResult || []).map((r) => `<div class="result">
      <div class="grow"><strong>Compte ${esc(r.id)}</strong> <span class="badge">${r.created ? "créé" : "mis à jour"}</span>
        <div class="help">Client ${esc(r.version)}${r.gateway ? " · passerelle " + esc(r.gateway) : ""} · clé de ${r.key_length} caractères</div></div>
      <button class="btn small" data-act="open-account" data-id="${esc(r.id)}">Ouvrir</button></div>`).join("");
    const copyOptions = S.accounts.map((a) => `<option value="${esc(a.id)}">${esc(displayName(a))}</option>`).join("");
    return `
      <div class="head"><div class="title"><h1>Ajouter un compte</h1>
        <div class="subtitle">Chaque compte a son fichier de configuration, son bot et son journal.</div></div></div>
      <div class="two">
        <section class="card">
          <h2>Importer une capture réseau</h2>
          <p class="help">Recommandé. La console retrouve toute seule le compte, la clé d'accès, la version du client et la passerelle.
            Si le compte existe déjà, seuls ses identifiants sont mis à jour et vos autres réglages sont conservés.</p>
          <label class="drop" id="drop"><input type="file" id="file" accept=".pcap,.pcapng,.cap">
            ${S.importing ? '<span class="spin"></span><strong>Analyse de la capture…</strong>'
              : "<strong>Choisir la capture</strong><span>ou déposer le fichier ici (.pcap, .pcapng)</span>"}</label>
          ${results}
          <details ${S.accounts.length ? "" : "open"}><summary>Comment faire la capture ?</summary>
            <ol>
              <li>Fermez complètement le jeu.</li>
              <li>${S.os === "windows" ? "Ouvrez PowerShell <strong>en administrateur</strong> et lancez :" : "Démarrez une capture du trafic TCP (Wireshark, tcpdump…), puis lancez le jeu."}
                ${S.os === "windows" ? codeBlock(CAPTURE_STEPS_WINDOWS) : ""}</li>
              <li>Démarrez la capture <strong>avant</strong> d'ouvrir le jeu, et arrêtez-la seulement une fois en jeu.</li>
              <li>Importez le fichier ici, puis <strong>supprimez-le</strong> : il contient la clé d'accès du compte.</li>
            </ol>
            <p class="help">Avec un VPN actif, ça fonctionne aussi. Les captures faites sur le PC ou dans un émulateur sont acceptées.</p>
          </details>
        </section>
        <section class="card">
          <h2>Créer un compte vide</h2>
          <p class="help">Pour saisir les identifiants à la main, ou préparer un compte avant d'importer sa capture.</p>
          <div class="fields">
            <div class="field"><label class="lbl" for="new-name">Nom du compte</label>
              <input type="text" id="new-name" maxlength="48" placeholder="ex. principal, ferme-1" spellcheck="false"></div>
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
    S.schema.categories.forEach((c) => {
      const tab = $(`.tab[data-tab="${c.id}"]`);
      if (!tab) return;
      const keys = c.fields.map((f) => f.key);
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
    if (!count) { bar.hidden = true; return; }
    const running = S.acc.status.state === "running";
    bar.innerHTML = `<strong>${count} modification${count > 1 ? "s" : ""} non enregistrée${count > 1 ? "s" : ""}</strong>
      <span class="spacer"></span>
      <button class="btn" data-act="discard">Annuler</button>
      <button class="btn ${running ? "" : "primary"}" data-act="save">Enregistrer</button>
      ${running ? '<button class="btn primary" data-act="save-restart">Enregistrer et redémarrer</button>' : ""}`;
    bar.hidden = false;
  }

  function refreshControls() {
    if (S.view !== "account" || !S.acc) return;
    const ctrl = $("#ctrl");
    if (ctrl) ctrl.innerHTML = ctrlHtml();
    const notes = $("#notes");
    if (notes) notes.innerHTML = notesHtml();
    renderSavebar();
  }

  // ------------------------------------------------------------------ logs

  function stopLogs() { clearTimeout(S.logTimer); S.logTimer = null; }

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
    try {
      const payload = await api("GET", `/api/accounts/${enc(id)}`);
      S.view = "account";
      S.id = id;
      S.acc = payload;
      S.form = initialForm(payload);
      S.errors = {};
      S.prefix = payload.values["command.prefix"] || "$";
      if (tab) S.tab = tab;
      try { sessionStorage.setItem("lmbot-last", id); } catch (_) { /* optional */ }
      render();
    } catch (error) {
      toast(error.message, "err");
    }
  }

  function showView(view) {
    if (!confirmDiscard()) return;
    S.view = view;
    S.acc = view === "account" ? S.acc : null;
    S.form = {};
    S.errors = {};
    if (view === "add") S.importResult = null;
    render();
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
        if (category) S.tab = category.id;
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
    const todo = S.accounts.filter((a) => a.status.state !== "running" && a.has_key && a.igg_id);
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
    for (const account of S.accounts.filter((a) => a.status.state === "running")) {
      try { await api("POST", `/api/accounts/${enc(account.id)}/stop`); } catch (error) { toast(error.message, "err"); }
    }
    toast("Tous les bots sont arrêtés");
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
      S.tab = "account";
      render();
      toast("Compte créé. Renseignez ses identifiants.", "ok");
    } catch (error) {
      toast(error.message, "err");
    }
  }

  async function saveSettings() {
    ["client", "stagger"].forEach((name) => { const el = $(`#set-${name}-err`); el.hidden = true; });
    try {
      S.settings = await api("PUT", "/api/settings", {
        client_path: $("#set-client").value,
        stagger: $("#set-stagger").value,
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

  function authProblem() {
    stopLogs();
    $("#app").innerHTML = `<main style="margin:10vh auto;max-width:520px;padding:24px"><div class="notice err">
      <p><strong>Accès refusé.</strong></p>
      <p>Ouvrez le lien affiché dans la fenêtre où le serveur a été lancé : il contient le jeton d'accès.</p></div></main>`;
  }

  const ACTIONS = {
    "open-account": (el) => openAccount(el.dataset.id, S.view === "account" ? undefined : "account"),
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
    alias: async () => {
      const summary = accountById(S.id);
      const alias = prompt("Nom affiché pour ce compte (vide pour revenir au nom par défaut) :", (summary && summary.alias) || "");
      if (alias === null) return;
      try { await api("PUT", `/api/accounts/${enc(S.id)}/alias`, { alias }); await refreshState(); render(); }
      catch (error) { toast(error.message, "err"); }
    },
    "delete-account": async () => {
      if (!confirm(`Supprimer définitivement le compte « ${S.id} » et ses identifiants ?`)) return;
      try {
        await api("DELETE", `/api/accounts/${enc(S.id)}`);
        toast("Compte supprimé");
        S.acc = null; S.form = {}; S.view = "welcome";
        await refreshState();
        if (S.accounts.length) await openAccount(S.accounts[0].id, "account"); else render();
      } catch (error) { toast(error.message, "err"); }
    },
    "create-empty": createEmpty,
    "start-all": startAll,
    "stop-all": stopAll,
    "save-settings": saveSettings,
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

  // ------------------------------------------------------------------ boot

  function initToken() {
    const match = /[#&]t=([^&]+)/.exec(location.hash);
    try {
      if (match) {
        sessionStorage.setItem("lmbot-token", match[1]);
        history.replaceState(null, "", location.pathname);
      }
      S.token = sessionStorage.getItem("lmbot-token");
    } catch (_) {
      S.token = match ? match[1] : null;
    }
  }

  async function boot() {
    initToken();
    if (!S.token) return authProblem();
    renderShell();
    document.addEventListener("click", onClick);
    document.addEventListener("input", onInput);
    document.addEventListener("change", (event) => {
      if (event.target.id === "file") importFile(event.target.files[0]);
    });
    document.addEventListener("keydown", (event) => {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "s" && S.view === "account" && dirtyKeys().length) {
        event.preventDefault();
        save(false);
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
    if (first) await openAccount(first.id, "account"); else render();
    setInterval(() => { if (!document.hidden) refreshState(); }, 3000);
  }

  boot();
})();
