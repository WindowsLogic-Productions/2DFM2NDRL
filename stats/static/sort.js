// Client-side table sorting + row exclusion for the public stats site.
//
// CSP-safe (external file, no inline JS). Two behaviours, both purely client-
// side (the data is already fully rendered server-side -- no fetch):
//
//  1. SORT -- any <table class="sortable"> gets clickable column headers.
//     Click to sort, click again to reverse. Numeric columns (.num/.rank)
//     sort numerically; text columns sort alphabetically (natural order).
//     The "#" column sorts by the original server order (so clicking it
//     restores the default ranking). After any sort, the visible rows are
//     renumbered 1..N so "#" always reflects the current ranking.
//
//  2. EXCLUDE -- a <table class="sortable" data-hideable> additionally gets a
//     small "✕" on each row (hover to reveal): click it to drop a character
//     from the list. Excluded rows vanish, the remaining rows are re-ranked
//     and re-counted, and a bar appears above the table listing what's hidden
//     with one-click chips to bring each back (or "show all"). The exclusion
//     set persists per game in localStorage, so noise rows (e.g. an unknown
//     "id 1") stay hidden across reloads until you restore them.
(function () {
  'use strict';

  function cellText(row, idx) {
    var cell = row.children[idx];
    return cell ? cell.textContent.trim() : '';
  }

  // Pull a comparable number out of a cell ("12", "57.3%", "1,024", "—").
  // Non-numeric / empty cells return -Infinity so "no winrate yet" rows fall
  // to the bottom on a descending sort.
  function asNumber(text) {
    if (text == null) return -Infinity;
    var cleaned = text.replace(/[^0-9.\-]/g, '');
    if (cleaned === '' || cleaned === '-' || cleaned === '.') return -Infinity;
    var n = parseFloat(cleaned);
    return isNaN(n) ? -Infinity : n;
  }

  function pad2(n) { return (n < 10 ? '0' : '') + n; }
  function rankCell(row) { return row.querySelector('td.rank') || row.children[0]; }
  function origIndex(row) { return parseInt(row.getAttribute('data-orig'), 10) || 0; }

  function gameId() {
    var m = window.location.pathname.match(/\/g\/([^\/]+)/);
    return m ? decodeURIComponent(m[1]) : '';
  }
  // Stable identity for persistence: the row's link href (e.g. the /c/ char
  // URL), falling back to its visible label.
  function rowKey(row) {
    var a = row.querySelector('td a[href]');
    if (a) return a.getAttribute('href');
    var c = row.children[1] || row.children[0];
    return c ? c.textContent.trim() : '';
  }
  function rowLabel(row) {
    var a = row.querySelector('td a');
    if (a) return a.textContent.trim();
    var c = row.children[1] || row.children[0];
    return c ? c.textContent.trim() : '?';
  }

  // Renumber the rank cell of every VISIBLE row to its current 1-based position.
  function renumber(table) {
    var body = table.tBodies[0];
    if (!body) return;
    var i = 0;
    Array.prototype.forEach.call(body.rows, function (r) {
      if (r.style.display === 'none') return;
      var rc = rankCell(r);
      if (rc) rc.textContent = pad2(++i);
    });
  }

  function sortBody(table, idx, mode, asc) {
    var body = table.tBodies[0];
    if (!body) return;
    var rows = Array.prototype.slice.call(body.rows);
    rows.sort(function (a, b) {
      var cmp;
      if (mode === 'rank') {
        cmp = origIndex(a) - origIndex(b);
      } else if (mode === 'num') {
        cmp = asNumber(cellText(a, idx)) - asNumber(cellText(b, idx));
      } else {
        cmp = cellText(a, idx).localeCompare(cellText(b, idx),
          undefined, { numeric: true, sensitivity: 'base' });
      }
      if (cmp === 0) return origIndex(a) - origIndex(b); // stable tiebreak
      return asc ? cmp : -cmp;
    });
    var frag = document.createDocumentFragment();
    rows.forEach(function (r) { frag.appendChild(r); });
    body.appendChild(frag);
    renumber(table);
  }

  function wireSort(table) {
    var head = table.tHead;
    if (!head || !head.rows[0]) return;
    var headers = head.rows[0].children;

    // Tag original server order once, so the "#" sort + tiebreaks are stable.
    var body = table.tBodies[0];
    if (body) {
      Array.prototype.forEach.call(body.rows, function (r, i) {
        if (r.getAttribute('data-orig') == null) r.setAttribute('data-orig', i);
      });
    }

    Array.prototype.forEach.call(headers, function (th, idx) {
      var isRank = th.classList.contains('rank');
      var numeric = th.classList.contains('num') || isRank;
      var mode = isRank ? 'rank' : (numeric ? 'num' : 'text');
      th.classList.add('sort-th');
      th.setAttribute('role', 'button');
      th.setAttribute('tabindex', '0');
      th.setAttribute('aria-label', 'Sort by ' + th.textContent.trim());

      function activate() {
        var dir = th.getAttribute('data-sort-dir');
        // Same column: flip. Fresh column: numeric defaults high->low, text A->Z.
        var asc = dir === 'desc' ? true : (dir === 'asc' ? false : !numeric);
        Array.prototype.forEach.call(headers, function (o) {
          o.removeAttribute('data-sort-dir');
          o.classList.remove('sort-asc', 'sort-desc');
        });
        th.setAttribute('data-sort-dir', asc ? 'asc' : 'desc');
        th.classList.add(asc ? 'sort-asc' : 'sort-desc');
        sortBody(table, idx, mode, asc);
      }

      th.addEventListener('click', activate);
      th.addEventListener('keydown', function (e) {
        if (e.key === 'Enter' || e.key === ' ' || e.key === 'Spacebar') {
          e.preventDefault();
          activate();
        }
      });
    });
  }

  function wireHide(table) {
    var body = table.tBodies[0];
    if (!body) return;

    var storeKey = 'statshide:' + gameId();
    var hidden;
    try { hidden = new Set(JSON.parse(localStorage.getItem(storeKey) || '[]')); }
    catch (e) { hidden = new Set(); }
    function persist() {
      try { localStorage.setItem(storeKey, JSON.stringify(Array.prototype.slice.call(hidden))); }
      catch (e) { /* private mode / quota -- exclusion still works for the session */ }
    }

    var bar = document.createElement('div');
    bar.className = 'hide-bar';
    table.parentNode.insertBefore(bar, table);

    function refresh() {
      var total = body.rows.length, shown = 0, excluded = [];
      Array.prototype.forEach.call(body.rows, function (r) {
        if (hidden.has(rowKey(r))) { r.style.display = 'none'; excluded.push(r); }
        else { r.style.display = ''; shown++; }
      });
      renumber(table);

      bar.textContent = '';
      if (excluded.length === 0) { bar.classList.remove('on'); return; }
      bar.classList.add('on');

      var status = document.createElement('span');
      status.className = 'hide-status';
      status.textContent = 'Showing ' + shown + ' of ' + total + ' · excluded: ';
      bar.appendChild(status);

      excluded.forEach(function (r) {
        var chip = document.createElement('button');
        chip.type = 'button';
        chip.className = 'hide-chip';
        chip.title = 'Restore ' + rowLabel(r);
        chip.appendChild(document.createTextNode(rowLabel(r) + ' '));
        var x = document.createElement('span');
        x.className = 'x';
        x.textContent = '↺'; // ↺ restore
        chip.appendChild(x);
        chip.addEventListener('click', function () {
          hidden.delete(rowKey(r)); persist(); refresh();
        });
        bar.appendChild(chip);
      });

      var reset = document.createElement('button');
      reset.type = 'button';
      reset.className = 'hide-reset';
      reset.textContent = 'show all';
      reset.addEventListener('click', function () {
        hidden.clear(); persist(); refresh();
      });
      bar.appendChild(reset);
    }

    // Per-row exclude button, tucked into the label cell (2nd column).
    Array.prototype.forEach.call(body.rows, function (r) {
      var cell = r.children[1] || r.children[0];
      if (!cell) return;
      var btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'row-hide';
      btn.title = 'Exclude ' + rowLabel(r);
      btn.setAttribute('aria-label', 'Exclude ' + rowLabel(r));
      btn.textContent = '✕'; // ✕
      btn.addEventListener('click', function (e) {
        e.preventDefault();
        e.stopPropagation();
        hidden.add(rowKey(r)); persist(); refresh();
      });
      cell.appendChild(btn);
    });

    refresh();
  }

  document.addEventListener('DOMContentLoaded', function () {
    var tables = document.querySelectorAll('table.sortable');
    Array.prototype.forEach.call(tables, function (t) {
      wireSort(t);
      if (t.hasAttribute('data-hideable')) wireHide(t);
    });
  });
})();
