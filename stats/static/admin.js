/* Admin editor — live inline editing for /admin/.
 *
 * Vanilla JS (no framework). All POSTs go to /admin/api/* which
 * mutate registry.json under fcntl lock and return JSON.
 *
 * Editing model:
 *   - Each row in the index table has an "edit" toggle that expands
 *     a panel below the row with all editable fields + the resources
 *     list.
 *   - Scalar fields auto-save on blur (PATCH /admin/api/g/{id}).
 *   - Resources can be added (form), edited (click any field), or
 *     removed (× button). Each mutation goes to /admin/api/g/{id}/
 *     resource/{idx}.
 *   - "+ NEW GAME" expands a card at the top; create posts to
 *     /admin/api/g and inserts the new row.
 *   - Delete + Merge are inline buttons in the expanded panel.
 *
 * Feedback:
 *   - Successful save → small green "✓" pulse fades next to the
 *     changed control.
 *   - Failure → red toast pinned bottom-right with the error string.
 */
(() => {
'use strict';

// ─── Toast / status badge ─────────────────────────────────────────

function toast(msg, kind = 'err') {
  const el = document.createElement('div');
  el.className = 'admin-toast admin-toast-' + kind;
  el.textContent = msg;
  document.body.appendChild(el);
  setTimeout(() => { el.classList.add('admin-toast-fade'); }, 2400);
  setTimeout(() => { el.remove(); }, 3000);
}

function flashSavedBadge(target) {
  const b = document.createElement('span');
  b.className = 'saved-pulse';
  b.textContent = '✓';
  target.parentNode.insertBefore(b, target.nextSibling);
  setTimeout(() => b.remove(), 1200);
}

// ─── HTTP helpers ─────────────────────────────────────────────────

async function api(method, path, body) {
  const init = {
    method,
    headers: { 'Accept': 'application/json' },
    credentials: 'same-origin',
  };
  if (body !== undefined) {
    init.headers['Content-Type'] = 'application/json';
    init.body = JSON.stringify(body);
  }
  const res = await fetch(path, init);
  if (!res.ok) {
    let detail = res.statusText;
    try {
      const j = await res.json();
      detail = j.detail || detail;
    } catch (_) {}
    throw new Error(`${res.status} ${detail}`);
  }
  return res.json();
}

// ─── Field auto-save ──────────────────────────────────────────────
//
// Any input/textarea/select inside .editor-panel with [data-field]
// attribute auto-saves on blur. The closest .editor-panel element
// owns the game_id (data-game-id).

function setSaveState(panel, state, msg) {
  const banner = panel.querySelector('.save-status');
  if (!banner) return;
  banner.dataset.state = state;
  const text = banner.querySelector('.save-status-text');
  if (text && msg) text.textContent = msg;
}

async function patchField(panel, gid, field, value, sourceEl) {
  setSaveState(panel, 'saving', `saving ${field}…`);
  try {
    const r = await api('PATCH', `/admin/api/g/${encodeURIComponent(gid)}`,
                        { [field]: value });
    if (r.noop) {
      setSaveState(panel, 'saved', 'no changes to save');
    } else {
      setSaveState(panel, 'saved', `saved ${field}`);
      if (sourceEl) flashSavedBadge(sourceEl);
    }
  } catch (e) {
    setSaveState(panel, 'error', `save failed: ${e.message}`);
    toast(`save failed: ${e.message}`);
  }
}

function bindAutoSave(panel) {
  const gid = panel.dataset.gameId;
  panel.querySelectorAll('[data-field]').forEach(input => {
    let lastSaved = input.value;
    const tag = input.tagName;
    // Selects fire change immediately (mobile-safe — blur is unreliable
    // when the user taps an option). Text inputs + textareas save on
    // blur as before, which keeps typing fluid without spamming the
    // server on every keystroke.
    const evtName = (tag === 'SELECT') ? 'change' : 'blur';
    input.addEventListener(evtName, async () => {
      if (input.value === lastSaved) return;
      lastSaved = input.value;
      await patchField(panel, gid, input.dataset.field, input.value, input);
    });
    // Mobile keyboards on Discord's in-app browser sometimes don't
    // fire blur reliably; pressing Enter in a text input commits too.
    if (tag === 'INPUT') {
      input.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') input.blur();
      });
    }
  });

  // Explicit "Save all" button — for editors who don't trust auto-
  // save or are on a flaky connection. Walks every dirty field and
  // PATCHes their values in one round.
  const saveBtn = panel.querySelector('.btn-save-all');
  if (saveBtn) {
    saveBtn.addEventListener('click', async () => {
      const body = {};
      panel.querySelectorAll('[data-field]').forEach(input => {
        body[input.dataset.field] = input.value;
      });
      setSaveState(panel, 'saving', 'saving all fields…');
      try {
        const r = await api('PATCH',
          `/admin/api/g/${encodeURIComponent(gid)}`, body);
        const n = (r.changed || []).length;
        setSaveState(panel, 'saved',
          n ? `saved ${n} field${n === 1 ? '' : 's'}` : 'no changes to save');
      } catch (e) {
        setSaveState(panel, 'error', `save failed: ${e.message}`);
      }
    });
  }
}

// ─── Resources list rendering ─────────────────────────────────────

function makeBadge(text, color) {
  const a = document.createElement('span');
  a.className = 'badge';
  if (color) a.style.background = color;
  a.textContent = text;
  return a;
}

function renderResourceRow(panel, gid, ridx, r) {
  const row = document.createElement('div');
  row.className = 'res-edit-row';
  row.dataset.ridx = ridx;

  const kindSel = document.createElement('select');
  ALLOWED_KINDS.forEach(k => {
    const opt = document.createElement('option');
    opt.value = k; opt.textContent = k;
    if (r.kind === k) opt.selected = true;
    kindSel.appendChild(opt);
  });
  kindSel.addEventListener('change', () => patchRes(gid, ridx, row, { kind: kindSel.value }));
  row.appendChild(kindSel);

  const urlIn = document.createElement('input');
  urlIn.type = 'url'; urlIn.value = r.url || '';
  urlIn.addEventListener('blur', () => patchRes(gid, ridx, row, { url: urlIn.value }));
  row.appendChild(urlIn);

  const labelIn = document.createElement('input');
  labelIn.type = 'text'; labelIn.value = r.label || ''; labelIn.placeholder = 'label';
  labelIn.addEventListener('blur', () => patchRes(gid, ridx, row, { label: labelIn.value }));
  row.appendChild(labelIn);

  const descIn = document.createElement('input');
  descIn.type = 'text'; descIn.value = r.desc || ''; descIn.placeholder = 'desc';
  descIn.addEventListener('blur', () => patchRes(gid, ridx, row, { desc: descIn.value }));
  row.appendChild(descIn);

  // Live link + wayback buttons
  if (r.url) {
    const live = document.createElement('a');
    live.href = r.url; live.target = '_blank'; live.rel = 'noopener';
    live.className = 'badge'; live.textContent = '↗ live';
    row.appendChild(live);
  }
  if (r.archive_url) {
    const wb = document.createElement('a');
    wb.href = r.archive_url; wb.target = '_blank'; wb.rel = 'noopener';
    wb.className = 'badge';
    wb.style.background = (r.preferred === 'wayback_closest' || r.preferred === 'wayback_calendar')
      ? 'rgba(120,120,160,0.4)' : 'rgba(80,160,80,0.4)';
    wb.textContent = (r.preferred === 'wayback_closest' || r.preferred === 'wayback_calendar')
      ? 'wayback?' : 'wayback ✓';
    row.appendChild(wb);
  }

  const del = document.createElement('button');
  del.className = 'btn-del'; del.textContent = '×';
  del.title = 'remove this resource';
  del.addEventListener('click', async () => {
    if (!confirm(`Remove ${r.kind} ${r.url}?`)) return;
    try {
      await api('DELETE', `/admin/api/g/${encodeURIComponent(gid)}/resource/${ridx}`);
      // Re-render the whole resource list since indices shift after delete.
      reloadResources(panel, gid);
    } catch (e) { toast(`delete failed: ${e.message}`); }
  });
  row.appendChild(del);

  return row;
}

async function patchRes(gid, ridx, row, body) {
  try {
    const res = await api('PATCH',
      `/admin/api/g/${encodeURIComponent(gid)}/resource/${ridx}`, body);
    if (res.ok && !res.noop) flashSavedBadge(row.firstChild);
  } catch (e) { toast(`patch failed: ${e.message}`); }
}

async function reloadResources(panel, gid) {
  // We don't have a bulk-fetch endpoint; instead, fire a search by
  // game_id and read the row back. Cheap enough at our scale.
  try {
    const res = await api('GET',
      `/admin/api/search?q=${encodeURIComponent(gid)}`);
    // The search returns metadata only (n_resources count). For the
    // full list, we re-render lazily by reloading the page section.
    // Simplest path: reload the panel from the server using the
    // existing /admin/g/{id} HTML page (saves duplicating template
    // logic in JS). We just re-init the resources list locally
    // after a delete by removing the deleted row and reindexing.
    // — keeping this no-op for now since deleteResource handles
    // visual removal directly.
  } catch (e) {}
}

function bindResourceList(panel) {
  const gid = panel.dataset.gameId;
  const list = panel.querySelector('.res-list');
  if (!list) return;

  // Remove handler is already wired per-row in renderResourceRow when
  // we replace the static markup. The static (server-rendered) rows
  // have data-ridx + control elements; here we wire them up.
  list.querySelectorAll('.res-edit-row').forEach(row => {
    const ridx = parseInt(row.dataset.ridx, 10);
    row.querySelector('select[data-rfield="kind"]').addEventListener(
      'change', e => patchRes(gid, ridx, row, { kind: e.target.value }));
    ['url', 'label', 'desc'].forEach(fld => {
      const el = row.querySelector(`input[data-rfield="${fld}"]`);
      if (el) el.addEventListener('blur',
        () => patchRes(gid, ridx, row, { [fld]: el.value }));
    });
    const del = row.querySelector('.btn-del');
    if (del) del.addEventListener('click', async () => {
      if (!confirm(`Remove this resource?`)) return;
      try {
        await api('DELETE',
          `/admin/api/g/${encodeURIComponent(gid)}/resource/${ridx}`);
        row.remove();
        // Reindex remaining rows so subsequent deletes hit the right idx.
        list.querySelectorAll('.res-edit-row').forEach((r, i) => {
          r.dataset.ridx = i;
        });
      } catch (e) { toast(`delete failed: ${e.message}`); }
    });
  });

  // Add-resource form
  const addForm = panel.querySelector('.res-add-form');
  if (addForm) {
    addForm.addEventListener('submit', async (e) => {
      e.preventDefault();
      const fd = new FormData(addForm);
      const body = {
        kind:  fd.get('kind') || 'other',
        url:   fd.get('url'),
        label: fd.get('label') || '',
      };
      try {
        const r = await api('POST',
          `/admin/api/g/${encodeURIComponent(gid)}/resource`, body);
        if (r.duplicate) {
          toast('already exists', 'info');
          return;
        }
        // Append a new editable row to the list
        const newRow = renderResourceRow(panel, gid, r.idx, r.resource);
        list.appendChild(newRow);
        addForm.reset();
        flashSavedBadge(addForm);
      } catch (e) { toast(`add failed: ${e.message}`); }
    });
  }
}

// ─── Versions list ────────────────────────────────────────────────

async function patchVer(gid, vidx, body) {
  return api('PATCH',
    `/admin/api/g/${encodeURIComponent(gid)}/version/${vidx}`, body);
}

function bindVersionList(panel) {
  const gid = panel.dataset.gameId;
  const list = panel.querySelector('.ver-list');
  if (!list) return;

  // Wire up auto-save on existing rows
  list.querySelectorAll('.ver-edit-row').forEach(row => {
    const vidx = parseInt(row.dataset.vidx, 10);
    row.querySelectorAll('input[data-vfield], select[data-vfield]')
       .forEach(el => {
      let last = el.value;
      const evt = el.tagName === 'SELECT' ? 'change' : 'blur';
      el.addEventListener(evt, async () => {
        if (el.value === last) return;
        last = el.value;
        try {
          const r = await patchVer(gid, vidx,
                                   { [el.dataset.vfield]: el.value });
          if (!r.noop) {
            flashSavedBadge(el);
            setSaveState(panel, 'saved',
              `saved version ${vidx} · ${el.dataset.vfield}`);
          }
        } catch (e) {
          setSaveState(panel, 'error', `version save failed: ${e.message}`);
          toast(`version save failed: ${e.message}`);
        }
      });
    });
    // Held toggle per version
    const held = row.querySelector('.btn-ver-held');
    if (held) held.addEventListener('click', async () => {
      held.disabled = true;
      try {
        const r = await api('POST',
          `/admin/api/g/${encodeURIComponent(gid)}/version/${vidx}/held/toggle`);
        held.classList.toggle('active', !!r.you_hold);
        held.textContent = r.you_hold ? '✓ you' : '+ I have';
        setSaveState(panel, 'saved',
          r.you_hold ? `you now hold version ${vidx}`
                     : `you released version ${vidx}`);
        // Update the "also held by" list
        const others = (r.held_by || []).filter(n => n !== window._editorNick);
        const heldList = row.querySelector('.ver-held-list');
        if (heldList) {
          heldList.innerHTML = '';
          others.forEach((n, i) => {
            const s = document.createElement('span');
            s.className = 'held-nick';
            s.textContent = n;
            heldList.appendChild(s);
            if (i < others.length - 1) heldList.appendChild(document.createTextNode(' '));
          });
        }
      } catch (e) {
        toast(`version held toggle failed: ${e.message}`);
      } finally {
        held.disabled = false;
      }
    });
    // Delete version
    const del = row.querySelector('.btn-del-version');
    if (del) del.addEventListener('click', async () => {
      const lbl = row.querySelector('input[data-vfield="label"]')?.value || '?';
      if (!confirm(`Remove version "${lbl}"?`)) return;
      try {
        await api('DELETE',
          `/admin/api/g/${encodeURIComponent(gid)}/version/${vidx}`);
        // Remove the row + its meta line if any
        const meta = row.nextElementSibling;
        row.remove();
        if (meta && meta.classList.contains('ver-meta')) meta.remove();
        // Reindex remaining rows
        list.querySelectorAll('.ver-edit-row').forEach((r, i) => {
          r.dataset.vidx = i;
        });
        setSaveState(panel, 'saved', `removed version "${lbl}"`);
      } catch (e) {
        toast(`delete failed: ${e.message}`);
      }
    });
  });

  // Add-version form
  const addForm = panel.querySelector('.ver-add-form');
  if (addForm) {
    addForm.addEventListener('submit', async (e) => {
      e.preventDefault();
      const fd = new FormData(addForm);
      const body = {};
      for (const [k, v] of fd.entries()) {
        if (typeof v === 'string' && v.trim()) body[k] = v.trim();
      }
      if (!body.label) {
        toast('label is required (e.g. "v0.1")');
        return;
      }
      try {
        const r = await api('POST',
          `/admin/api/g/${encodeURIComponent(gid)}/version`, body);
        if (r.duplicate) {
          toast('version already exists', 'info');
          return;
        }
        // Reload the page to render the new row + bind handlers — easier
        // than rebuilding the row in JS with all its state.
        setSaveState(panel, 'saved', `added version "${body.label}"`);
        setTimeout(() => location.reload(), 400);
      } catch (e) {
        toast(`add version failed: ${e.message}`);
      }
    });
  }
}


// ─── "I have this locally" toggle ─────────────────────────────────

function bindHeldToggle(panel) {
  const btn = panel.querySelector('.btn-held-toggle');
  if (!btn) return;
  const gid = btn.dataset.gameId;
  btn.addEventListener('click', async () => {
    btn.disabled = true;
    try {
      const r = await api('POST',
        `/admin/api/g/${encodeURIComponent(gid)}/held/toggle`);
      btn.classList.toggle('active', !!r.you_hold);
      btn.textContent = r.you_hold ? '✓ You hold a copy' : '+ I have this';
      btn.dataset.youHold = r.you_hold ? '1' : '0';
      // Refresh the held-by list display
      const list = panel.querySelector('.held-list');
      if (list) {
        const me = btn.closest('label').querySelector('.btn-held-toggle')
                       .dataset.you || '';
        const others = (r.held_by || []).filter(n => n !== me);
        list.innerHTML = '';
        if ((r.held_by || []).length === 0) {
          const span = document.createElement('span');
          span.className = 'muted';
          span.textContent = 'no one yet';
          list.appendChild(span);
        } else if (others.length > 0) {
          list.appendChild(document.createTextNode('Also held by: '));
          others.forEach((n, i) => {
            const s = document.createElement('span');
            s.className = 'held-nick';
            s.textContent = n;
            list.appendChild(s);
            if (i < others.length - 1) list.appendChild(document.createTextNode(', '));
          });
        }
      }
      setSaveState(panel, 'saved',
        r.you_hold ? `marked: you hold ${gid}` : `marked: you released ${gid}`);
    } catch (e) {
      toast(`held toggle failed: ${e.message}`);
    } finally {
      btn.disabled = false;
    }
  });
}


// ─── Delete + Merge ───────────────────────────────────────────────

function bindDangerZone(panel) {
  const gid = panel.dataset.gameId;

  // Rename game_id — the "old" id lands in aliased_ids[] so match
  // history doesn't orphan, and /g/<old_id> 301s to /g/<new_id>.
  const renameBtn = panel.querySelector('.btn-rename');
  if (renameBtn) renameBtn.addEventListener('click', async () => {
    const newId = prompt(
      `Rename game_id from "${gid}" to:\n\n` +
      `Allowed: lowercase letters, digits, hyphens. Old id is preserved as an alias so existing match history keeps working.`,
      gid);
    if (!newId || newId.trim() === gid) return;
    try {
      const r = await api('POST',
        `/admin/api/g/${encodeURIComponent(gid)}/rename`,
        { new_id: newId.trim() });
      toast(`renamed → ${r.new_id} (old id aliased)`, 'ok');
      // Reload so the page reflects the new id everywhere.
      setTimeout(() => location.assign(
        `/admin/?q=${encodeURIComponent(r.new_id)}`), 600);
    } catch (e) {
      toast(`rename failed: ${e.message}`);
    }
  });

  const delBtn = panel.querySelector('.btn-delete-game');
  if (delBtn) delBtn.addEventListener('click', async () => {
    const typed = prompt(
      `Type the game_id to confirm permanent delete:\n\n${gid}`);
    if (typed !== gid) {
      if (typed !== null) toast('game_id mismatch — aborted');
      return;
    }
    try {
      await api('DELETE',
        `/admin/api/g/${encodeURIComponent(gid)}?confirm=${encodeURIComponent(gid)}`);
      // Remove the table row and panel from the DOM
      const tr = panel.previousElementSibling;
      panel.remove();
      if (tr) tr.remove();
      toast('deleted', 'ok');
    } catch (e) { toast(`delete failed: ${e.message}`); }
  });

  const mergeBtn = panel.querySelector('.btn-merge');
  const mergeArea = panel.querySelector('.merge-area');
  const mergeIn = panel.querySelector('.merge-target-input');
  const mergeRes = panel.querySelector('.merge-results');
  if (mergeBtn && mergeArea) {
    mergeBtn.addEventListener('click', () => {
      mergeArea.classList.toggle('expanded');
    });
  }
  if (mergeIn && mergeRes) {
    let timer = null;
    mergeIn.addEventListener('input', () => {
      clearTimeout(timer);
      timer = setTimeout(async () => {
        const q = mergeIn.value.trim();
        if (q.length < 2) { mergeRes.innerHTML = ''; return; }
        try {
          const r = await api('GET',
            `/admin/api/search?q=${encodeURIComponent(q)}`);
          mergeRes.innerHTML = '';
          r.results
            .filter(x => x.game_id !== gid)
            .slice(0, 8)
            .forEach(x => {
              const li = document.createElement('div');
              li.className = 'merge-candidate';
              li.innerHTML = `<code>${x.game_id}</code> · ${x.name || '?'}`;
              const into = document.createElement('button');
              into.textContent = 'into target →';
              into.title = 'keep target row, drop this row';
              into.addEventListener('click', () => doMerge(gid, x.game_id, 'into_target'));
              li.appendChild(into);
              const self = document.createElement('button');
              self.textContent = '← absorb here';
              self.title = 'keep this row, drop target';
              self.addEventListener('click', () => doMerge(gid, x.game_id, 'into_self'));
              li.appendChild(self);
              mergeRes.appendChild(li);
            });
        } catch (e) { toast(`search failed: ${e.message}`); }
      }, 250);
    });
  }
}

async function doMerge(srcId, targetId, direction) {
  if (!confirm(
    `Merge ${srcId} ${direction === 'into_target' ? '→' : '←'} ${targetId}?`)) return;
  try {
    const r = await api('POST',
      `/admin/api/g/${encodeURIComponent(srcId)}/merge`,
      { target_id: targetId, direction });
    toast(`merged into ${r.kept_id}`, 'ok');
    // Reload the page so the table reflects the dropped row.
    setTimeout(() => location.reload(), 600);
  } catch (e) { toast(`merge failed: ${e.message}`); }
}

// ─── New game card ────────────────────────────────────────────────

function bindNewGameCard() {
  const form = document.querySelector('.new-game-form');
  if (!form) return;
  form.addEventListener('submit', async (e) => {
    e.preventDefault();
    const fd = new FormData(form);
    const body = {};
    for (const [k, v] of fd.entries()) body[k] = v;
    try {
      const r = await api('POST', '/admin/api/g', body);
      toast(`created ${r.game_id}`, 'ok');
      setTimeout(() => location.reload(), 600);
    } catch (e) { toast(`create failed: ${e.message}`); }
  });
}

// ─── Boot ─────────────────────────────────────────────────────────
//
// CSP blocks inline <script> tags (script-src 'self'), so the toggle
// wiring + ALLOWED_KINDS injection both live here in the external
// file. The template publishes ALLOWED_KINDS via a data-kinds JSON
// attribute on <body>; we parse it on boot.

function bindToggles() {
  document.querySelectorAll('.edit-toggle').forEach(btn => {
    btn.addEventListener('click', () => {
      const panel = document.getElementById(btn.dataset.target);
      if (!panel) return;
      const open = panel.classList.toggle('expanded');
      btn.classList.toggle('active', open);
    });
  });

  const newBtn = document.getElementById('toggle-new');
  const newCard = document.querySelector('.new-game-card');
  if (newBtn && newCard) {
    newBtn.addEventListener('click', () => {
      newCard.classList.toggle('expanded');
    });
  }

  // Filter dropdowns auto-submit their parent form on change.
  // Inline onchange="..." is blocked by CSP script-src 'self', so we
  // wire it externally here.
  document.querySelectorAll('select.auto-submit').forEach(sel => {
    sel.addEventListener('change', () => {
      if (sel.form) sel.form.submit();
    });
  });
}

function loadAllowedKinds() {
  const el = document.getElementById('admin-data');
  const raw = el ? el.dataset.kinds : null;
  if (!raw) { window.ALLOWED_KINDS = []; return; }
  try { window.ALLOWED_KINDS = JSON.parse(raw); }
  catch (_) { window.ALLOWED_KINDS = []; }
}

document.addEventListener('DOMContentLoaded', () => {
  loadAllowedKinds();
  bindToggles();
  // Pull editor nick from the data div so version-held updates can
  // filter "you" out of the "also held by" list.
  const dataDiv = document.getElementById('admin-data');
  window._editorNick = (dataDiv && dataDiv.dataset.editor) || '';
  document.querySelectorAll('.editor-panel').forEach(panel => {
    bindAutoSave(panel);
    bindResourceList(panel);
    bindVersionList(panel);
    bindDangerZone(panel);
    bindHeldToggle(panel);
  });
  bindNewGameCard();
});

})();
