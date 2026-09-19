'use strict';
'require baseclass';
'require rpc';
'require uci';

/*
 * 75_antilag.js  –  Antilag status widget for the LuCI Overview page.
 *
 * State logic (in priority order):
 *   1. /var/run/antilag.json exists and parses → daemon is running → show stats table
 *   2. UCI enabled = 1 but no status file     → enabled but not started / crashed
 *   3. UCI enabled = 0                         → disabled
 */

var callFileRead = rpc.declare({
    object: 'file',
    method: 'read',
    params: [ 'path' ],
    expect: { data: '' }
});

var callFileStat = rpc.declare({
    object: 'file',
    method: 'stat',
    params: [ 'path' ],
    expect: { type: '' }
});

/* ── Formatting helpers ───────────────────────────────────────── */

function fmtKbps(kbps) {
    if (kbps >= 1000)
        return (kbps / 1000).toFixed(1) + ' Mbps';
    return kbps + ' kbps';
}

function fmtOwd(ms10) {
    var sign = ms10 >= 0 ? '+' : '';
    return sign + (ms10 / 10).toFixed(1) + ' ms';
}

function fmtUptime(s) {
    if (s < 60)   return s + 's';
    if (s < 3600) return Math.floor(s / 60) + 'm ' + (s % 60) + 's';
    var h = Math.floor(s / 3600);
    var m = Math.floor((s % 3600) / 60);
    return h + 'h ' + m + 'm';
}

function loadCell(load, bb) {
    if (bb)
        return E('td', { 'class': 'td', 'style': 'color:#c00;font-weight:bold' },
                 'Bufferbloat');
    var map = {
        high:    [ 'color:#c07700;font-weight:bold', '▲ High'   ],
        low:     [ 'color:#888',                     '▼ Low'    ],
        running: [ 'color:#1a7f1a',                  '● Normal' ]
    };
    var e = map[load] || [ '', '— Idle' ];
    return E('td', { 'class': 'td', 'style': e[0] }, e[1]);
}

/* ── Table builders ──────────────────────────────────────────── */

function buildStatsTable(st) {
    var stateColors = { running: '#1a7f1a', idle: '#888', stall: '#c00' };
    var color = stateColors[st.state] || '#888';
    var label = st.state ? (st.state.charAt(0).toUpperCase() + st.state.slice(1)) : '?';
    var owdDlStyle = (st.avg_owd_dl_ms10 > 100) ? 'color:#c00' : 'color:#555';
    var owdUlStyle = (st.avg_owd_ul_ms10 > 100) ? 'color:#c00' : 'color:#555';

    return E('table', { 'class': 'table', 'id': 'antilag_status_table' }, [
        E('tr', { 'class': 'tr table-titles' }, [
            E('th', { 'class': 'th' }, _('Status')),
            E('th', { 'class': 'th' }, _('DL Shaped')),
            E('th', { 'class': 'th' }, _('DL Actual')),
            E('th', { 'class': 'th' }, _('DL Load')),
            E('th', { 'class': 'th' }, _('OWD DL Δ')),
            E('th', { 'class': 'th' }, _('UL Shaped')),
            E('th', { 'class': 'th' }, _('UL Actual')),
            E('th', { 'class': 'th' }, _('UL Load')),
            E('th', { 'class': 'th' }, _('OWD UL Δ')),
            E('th', { 'class': 'th' }, _('Uptime'))
        ]),
        E('tr', { 'class': 'tr' }, [
            E('td', { 'class': 'td', 'style': 'font-weight:bold;color:' + color }, label),
            E('td', { 'class': 'td', 'style': 'font-weight:bold' }, fmtKbps(st.shaper_dl_kbps)),
            E('td', { 'class': 'td', 'style': 'color:#555' },       fmtKbps(st.achieved_dl_kbps)),
            loadCell(st.load_dl, st.bb_dl),
            E('td', { 'class': 'td', 'style': owdDlStyle },         fmtOwd(st.avg_owd_dl_ms10)),
            E('td', { 'class': 'td', 'style': 'font-weight:bold' }, fmtKbps(st.shaper_ul_kbps)),
            E('td', { 'class': 'td', 'style': 'color:#555' },       fmtKbps(st.achieved_ul_kbps)),
            loadCell(st.load_ul, st.bb_ul),
            E('td', { 'class': 'td', 'style': owdUlStyle },         fmtOwd(st.avg_owd_ul_ms10)),
            E('td', { 'class': 'td', 'style': 'color:#555' },       fmtUptime(st.uptime_s))
        ])
    ]);
}

function buildSimpleTable(text, color) {
    return E('table', { 'class': 'table', 'id': 'antilag_status_table' }, [
        E('tr', { 'class': 'tr table-titles' }, [
            E('th', { 'class': 'th' }, _('Status'))
        ]),
        E('tr', { 'class': 'tr' }, [
            E('td', { 'class': 'td', 'style': 'color:' + color + ';font-weight:bold' }, text)
        ])
    ]);
}

/* ── State resolution ────────────────────────────────────────── */

function resolveState(fileExists, raw, uciEnabled) {
    var st = null;
    try { if (raw) st = JSON.parse(raw); } catch (e) {}

    if (fileExists && st && st.state)
        return buildStatsTable(st);

    if (fileExists)
        return buildSimpleTable(_('Active'), '#1a7f1a');

    if (uciEnabled)
        return buildSimpleTable(_('Enabled – not running'), '#c07700');

    return buildSimpleTable(_('Disabled'), '#888');
}

/* ── UCI enabled check ───────────────────────────────────────── */

function getUciEnabled() {
    var sections = uci.sections('antilag', 'antilag');
    for (var i = 0; i < sections.length; i++) {
        if (sections[i].enabled === '1')
            return true;
    }
    return false;
}

/* ── Poller ──────────────────────────────────────────────────── */

var POLL_MS = 2500;

function startPoller(container) {
    function poll() {
        Promise.all([
            uci.load('antilag'),
            callFileStat('/var/run/antilag.json').catch(function() { return ''; }),
            callFileRead('/var/run/antilag.json').catch(function() { return ''; })
        ]).then(function(results) {
            var fileExists = !!(results[1]);   /* non-empty type = file exists */
            var raw        = results[2] || '';
            var uciEnabled = getUciEnabled();
            var node       = resolveState(fileExists, raw, uciEnabled);

            while (container.firstChild)
                container.removeChild(container.firstChild);
            container.appendChild(node);
        });
    }

    poll();
    return setInterval(poll, POLL_MS);
}

/* ── Widget ──────────────────────────────────────────────────── */

return baseclass.extend({
    title: _('Antilag'),

    load: function() {
        return Promise.all([
            uci.load('antilag'),
            callFileStat('/var/run/antilag.json').catch(function() { return ''; }),
            callFileRead('/var/run/antilag.json').catch(function() { return ''; })
        ]);
    },

    render: function(data) {
        var fileExists = !!(data[1]);
        var raw        = data[2] || '';
        var uciEnabled = getUciEnabled();
        var container  = E('div', {}, [ resolveState(fileExists, raw, uciEnabled) ]);

        var pollInterval = startPoller(container);

        /* Cancel poller when the widget node leaves the DOM */
        var observer = new MutationObserver(function(mutations) {
            mutations.forEach(function(m) {
                m.removedNodes.forEach(function(node) {
                    if (node === container ||
                        (node.contains && node.contains(container))) {
                        clearInterval(pollInterval);
                        observer.disconnect();
                    }
                });
            });
        });
        requestAnimationFrame(function() {
            if (container.parentNode)
                observer.observe(container.parentNode, { childList: true });
        });

        return container;
    }
});
