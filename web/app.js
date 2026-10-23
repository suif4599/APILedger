import createAPILedger from './apiledger.js';

const DB_NAME = 'apiledger';
const DB_VERSION = 1;
const STORE = 'state';
const KEY = 'main';

const state = {
  module: null,
  user: null,
  owner: null,
  cloudOn: false,
  syncing: false,
  deleteStart: '',
  deleteEnd: '',
};

const $ = id => document.getElementById(id);

function showError(message) {
  const el = $('error');
  el.textContent = message || '';
  el.hidden = !message;
}

function setCloudStatus(text) {
  $('cloud-status').textContent = text;
}

function openDb() {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(DB_NAME, DB_VERSION);
    request.onupgradeneeded = () => request.result.createObjectStore(STORE);
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
}

async function readSaved() {
  try {
    const db = await openDb();
    return await new Promise((resolve, reject) => {
      const request = db.transaction(STORE, 'readonly').objectStore(STORE).get(KEY);
      request.onsuccess = () => resolve(request.result || null);
      request.onerror = () => reject(request.error);
    });
  } catch (error) {
    return null;
  }
}

async function writeSaved(record) {
  const db = await openDb();
  await new Promise((resolve, reject) => {
    const tx = db.transaction(STORE, 'readwrite');
    tx.objectStore(STORE).put(record, KEY);
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
  });
}

function bytesIntoHeap(bytes) {
  const pointer = state.module._malloc(bytes.length);
  state.module.HEAPU8.set(bytes, pointer);
  return pointer;
}

function cString(text) {
  return bytesIntoHeap(new TextEncoder().encode(text + '\0'));
}

function parsePointer(pointer) {
  return JSON.parse(state.module.UTF8ToString(pointer));
}

function snapshotBytes() {
  const lengthPointer = state.module._malloc(4);
  const dataPointer = state.module._apiledger_snapshot(lengthPointer);
  if (!dataPointer) {
    state.module._free(lengthPointer);
    return null;
  }
  const length = state.module.HEAP32[lengthPointer >> 2];
  const bytes = state.module.HEAPU8.slice(dataPointer, dataPointer + length);
  state.module._free(lengthPointer);
  return bytes;
}

async function persist(force = false) {
  if (state.cloudOn && !force) return;
  const bytes = snapshotBytes();
  if (!bytes) return;
  await writeSaved({ bytes, owner: state.owner, savedAt: Date.now() });
}

async function clearSaved() {
  try {
    const db = await openDb();
    await new Promise((resolve, reject) => {
      const tx = db.transaction(STORE, 'readwrite');
      tx.objectStore(STORE).delete(KEY);
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
    });
  } catch (error) {
    // keep the cloud path usable even when IndexedDB is unavailable
  }
}

let reportMounted = false;

function applyDeleteDefaults(report) {
  const summary = report.summary || {};
  const start = summary.date_min || '';
  const end = summary.date_max || '';
  const startInput = $('d-start');
  const endInput = $('d-end');

  if (!start || !end) {
    if (!summary.record_count) {
      startInput.value = '';
      endInput.value = '';
      state.deleteStart = '';
      state.deleteEnd = '';
    }
    return;
  }
  if (!startInput.value || startInput.value === state.deleteStart) startInput.value = start;
  if (!endInput.value || endInput.value === state.deleteEnd) endInput.value = end;
  state.deleteStart = start;
  state.deleteEnd = end;
}

function renderReport(initial) {
  const report = parsePointer(state.module._apiledger_report_json());
  if (report.ok === false) {
    showError(report.error);
    return;
  }
  if (initial && !reportMounted) {
    window.APILedgerReport.mount(report);
    reportMounted = true;
  } else {
    window.APILedgerReport.update(report);
  }
  applyDeleteDefaults(report);
}

function showImportResult(result) {
  const files = result.files || [];
  const written = files.reduce((sum, file) => sum + (file.written || 0), 0);
  const dropped = files.reduce((sum, file) => sum + (file.dropped || 0), 0);
  let status = `${files.length} file(s) · ${written} rows written`;
  if (dropped) status += ` · kept ${dropped} existing row(s)`;
  $('import-status').textContent = status;

  const skipped = result.skipped || [];
  const list = $('skipped');
  list.textContent = '';
  if (!skipped.length) {
    list.hidden = true;
    return;
  }
  for (const item of skipped) {
    const entry = document.createElement('li');
    const name = document.createElement('span');
    name.className = 'mono';
    name.textContent = item.name + ': ';
    entry.appendChild(name);
    entry.appendChild(document.createTextNode(item.reason));
    list.appendChild(entry);
  }
  list.hidden = false;
}

function wireImport() {
  const input = $('file');
  const button = $('import');

  button.addEventListener('click', () => input.click());

  input.addEventListener('change', async () => {
    const file = input.files[0];
    if (!file) return;
    if (state.syncing) {
      input.value = '';
      return;
    }
    button.disabled = true;
    showError('');
    $('skipped').hidden = true;
    $('import-status').textContent = 'Importing…';

    try {
      const bytes = new Uint8Array(await file.arrayBuffer());
      const namePointer = cString(file.name);
      const dataPointer = bytesIntoHeap(bytes);
      const result = parsePointer(
        state.module._apiledger_import(namePointer, dataPointer, bytes.length));
      state.module._free(namePointer);
      state.module._free(dataPointer);

      if (!result.ok) throw new Error(result.error || 'Import failed.');
      showImportResult(result);
      await persist();
      renderReport(false);
      await syncIfEnabled();
    } catch (error) {
      showError(error instanceof Error ? error.message : String(error));
      $('import-status').textContent = 'Import failed.';
    } finally {
      input.value = '';
      button.disabled = false;
    }
  });
}

function wireDelete() {
  const button = $('delete-range');

  button.addEventListener('click', async () => {
    const start = $('d-start').value;
    const end = $('d-end').value;
    if (!start || !end) {
      showError('Choose both dates for the period to delete.');
      return;
    }
    if (start > end) {
      showError('The start date must not be after the end date.');
      return;
    }
    if (!confirm(`Delete all rows from ${start} to ${end}? Cloud history for this period is removed when sync is on.`)) {
      return;
    }

    button.disabled = true;
    showError('');
    $('delete-status').textContent = 'Deleting…';

    const startPointer = cString(start);
    const endPointer = cString(end);
    try {
      const result = parsePointer(
        state.module._apiledger_delete_range(startPointer, endPointer));
      if (!result.ok) throw new Error(result.error || 'Delete failed.');
      await persist();
      renderReport(false);
      await syncIfEnabled();
      $('delete-status').textContent = `${result.deleted} row(s) deleted`;
    } catch (error) {
      showError(error instanceof Error ? error.message : String(error));
      $('delete-status').textContent = 'Delete failed.';
    } finally {
      state.module._free(startPointer);
      state.module._free(endPointer);
      button.disabled = false;
    }
  });
}

async function fetchSession() {
  try {
    const response = await fetch('/api/auth/session');
    if (!response.ok) return null;
    const data = await response.json();
    return data.authenticated ? data : null;
  } catch (error) {
    return null;
  }
}

async function applyServerState(stateJson) {
  const bytes = new TextEncoder().encode(stateJson);
  const pointer = bytesIntoHeap(bytes);
  const result = parsePointer(state.module._apiledger_apply_state(pointer, bytes.length));
  state.module._free(pointer);
  if (!result.ok) throw new Error(result.error || 'Sync result could not be applied.');
}

function chunkSources(sources) {
  const chunks = [];
  let current = [];
  let rows = 0;
  for (const source of sources) {
    const count = (source.rows || []).length;
    if (current.length && (current.length >= 50 || rows + count > 3000)) {
      chunks.push(current);
      current = [];
      rows = 0;
    }
    current.push(source);
    rows += count;
  }
  if (current.length) chunks.push(current);
  return chunks.length ? chunks : [[]];
}

async function syncNow() {
  if (!state.user || !state.cloudOn || state.syncing) return;
  state.syncing = true;
  $('import').disabled = true;
  $('delete-range').disabled = true;
  setCloudStatus('Syncing…');

  try {
    const dirty = parsePointer(state.module._apiledger_export_dirty());
    let merged = null;
    for (const sources of chunkSources(dirty.sources || [])) {
      const response = await fetch('api/sync', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-Requested-With': 'fetch' },
        body: JSON.stringify({ sources }),
      });
      if (!response.ok) throw new Error(`sync failed (${response.status})`);
      merged = await response.json();
      if (!merged.ok) throw new Error(merged.error || 'sync failed');
    }
    if (!merged) throw new Error('sync failed');

    await applyServerState(JSON.stringify(merged));
    state.owner = state.user.username;
    await clearSaved();
    renderReport(false);
    setCloudStatus('Cloud sync on — history is stored only in the cloud.');
  } catch (error) {
    setCloudStatus('Cloud sync failed — it will retry after the next change.');
  } finally {
    state.syncing = false;
    $('import').disabled = false;
    $('delete-range').disabled = false;
  }
}

async function syncIfEnabled() {
  if (state.cloudOn && state.user) await syncNow();
}

async function initCloud() {
  state.user = await fetchSession();
  const control = $('cloud-control');

  if (!state.user) {
    control.hidden = true;
    setCloudStatus('History is stored in this browser only.');
    return;
  }

  if (state.owner && state.owner !== state.user.username) {
    control.hidden = true;
    setCloudStatus(`Local history belongs to ${state.owner}. Cloud sync is off for this account.`);
    return;
  }

  control.hidden = false;
  state.cloudOn = localStorage.getItem('apiledger-cloud') !== 'off';
  $('cloud-toggle').checked = state.cloudOn;
  $('cloud-toggle').addEventListener('change', async event => {
    state.cloudOn = event.target.checked;
    localStorage.setItem('apiledger-cloud', state.cloudOn ? 'on' : 'off');
    if (state.cloudOn) {
      await syncNow();
    } else {
      await persist(true);
      setCloudStatus('History is stored in this browser only.');
    }
  });

  if (state.cloudOn) {
    await syncNow();
  } else {
    setCloudStatus('History is stored in this browser only.');
  }
}

async function boot() {
  try {
    state.module = await createAPILedger({ locateFile: file => file });
    if (state.module._apiledger_init() !== 0) {
      throw new Error('The ledger engine failed to start.');
    }

    const saved = await readSaved();
    if (saved && saved.bytes) {
      const bytes = saved.bytes instanceof Uint8Array ? saved.bytes : new Uint8Array(saved.bytes);
      const pointer = bytesIntoHeap(bytes);
      const ok = state.module._apiledger_load_state(pointer, bytes.length);
      state.module._free(pointer);
      if (ok !== 0) showError('The saved ledger could not be read. Import a bill to start a new one.');
      state.owner = saved.owner || null;
    }

    renderReport(true);
    wireImport();
    wireDelete();
    $('import').disabled = false;
    $('delete-range').disabled = false;
    await initCloud();
  } catch (error) {
    showError(error instanceof Error ? error.message : String(error));
  }
}

boot();
