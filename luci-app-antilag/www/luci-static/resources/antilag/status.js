'use strict';
'require baseclass';
'require rpc';
'require uci';

/*
 * antilag/status.js  –  shared Antilag live-status renderer.
 *
 * Used by both the LuCI Overview widget (status/include/75_antilag.js)
 * and the Antilag settings page.  render(container) fills <container>
 * with one status block per UCI 'antilag' section and polls every
 * POLL_MS.  A MutationObserver stops the poller when the container
 * leaves the DOM (LuCI swaps status includes / page nodes).
 *
 * Multi-instance: every UCI 'antilag' section runs its own daemon
 * process with its own status file (/var/run/antilag-<section>.json).
 *
 * Per-instance state logic (in priority order):
 *   1. status file exists and parses   → daemon running → stats + tin tables
 *   2. UCI enabled = 1 but no file     → enabled but not started / crashed
 *   3. UCI enabled = 0                 → disabled
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

function fmtPkts(n) {
    if (n >= 1000000) return (n / 1000000).toFixed(1) + 'M';
    if (n >= 1000)    return (n / 1000).toFixed(1) + 'k';
    return String(n);
}

function fmtBytes(b) {
    if (b >= 1073741824) return (b / 1073741824).toFixed(1) + ' GiB';
    if (b >= 1048576)    return (b / 1048576).toFixed(1) + ' MiB';
    if (b >= 1024)       return (b / 1024).toFixed(1) + ' KiB';
    return b + ' B';
}

function fmtBps(bps) {
    if (bps >= 1000000) return (bps / 1000000).toFixed(1) + ' Mbps';
    if (bps >= 1000)    return (bps / 1000).toFixed(1) + ' kbps';
    return bps + ' bps';
}

function fmtUs(us) {
    if (us >= 1000) return (us / 1000).toFixed(1) + ' ms';
    return us + ' µs';
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

/*
 * buildTinTable – live per-tin CAKE statistics (tc -s qdisc show style)
 * for one direction. Data comes from the kernel via the daemon's status
 * JSON ("cake_dl" / "cake_ul" sections, RTM_GETQDISC dump).
 */
function buildTinTable(title, qd) {
    var rows = [
        E('tr', { 'class': 'tr table-titles' }, [
            E('th', { 'class': 'th' }, _('Tin')),
            E('th', { 'class': 'th' }, _('Threshold')),
            E('th', { 'class': 'th' }, _('Sent')),
            E('th', { 'class': 'th' }, _('Dropped')),
            E('th', { 'class': 'th' }, _('ECN Marks')),
            E('th', { 'class': 'th' }, _('Backlog')),
            E('th', { 'class': 'th' }, _('Avg Delay'))
        ])
    ];

    for (var i = 0; i < qd.tin_cnt; i++) {
        var t = qd.tins[i];
        rows.push(E('tr', { 'class': 'tr' }, [
            E('td', { 'class': 'td' }, 'T' + (i + 1)),
            E('td', { 'class': 'td' }, fmtBps(t.threshold_bps)),
            E('td', { 'class': 'td' },
                fmtPkts(t.sent_packets) + ' / ' + fmtBytes(t.sent_bytes)),
            E('td', {
                'class': 'td',
                'style': t.dropped_packets ? 'color:#c00' : 'color:#555'
            }, t.dropped_packets + ' / ' + fmtBytes(t.dropped_bytes)),
            E('td', { 'class': 'td' },
                t.ecn_packets ? fmtPkts(t.ecn_packets) : '—'),
            E('td', { 'class': 'td' }, fmtBytes(t.backlog_bytes)),
            E('td', { 'class': 'td', 'style': 'color:#555' }, fmtUs(t.avg_delay_us))
        ]));
    }

    var header = [ E('h3', {}, _('CAKE tin stats') + ' – ' + title + ' (' + qd.kind + ')') ];
    var meta   = [];
    if (qd.capacity_bps)
        meta.push(_('Capacity estimate') + ': ' + fmtBps(qd.capacity_bps));
    if (qd.memory_limit)
        meta.push(_('Memory') + ': ' + fmtBytes(qd.memory_used) + ' / ' + fmtBytes(qd.memory_limit));
    if (meta.length)
        header.push(E('div', { 'style': 'color:#555;margin-bottom:0.4em' }, meta.join(' · ')));

    return E('div', { 'class': 'cbi-section', 'style': 'margin-top:0.5em' },
        header.concat([ E('table', { 'class': 'table' }, rows) ]));
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

/* ── Per-instance status block ───────────────────────────────── */

function statusPath(sid) {
    return '/var/run/antilag-' + sid + '.json';
}

function instanceEnabled(sid) {
    return uci.get('antilag', sid, 'enabled') === '1';
}

function fetchInstance(sid) {
    return Promise.all([
        callFileStat(statusPath(sid)).catch(function() { return ''; }),
        callFileRead(statusPath(sid)).catch(function() { return ''; })
    ]).then(function(res) {
        return { sid: sid, exists: !!res[0], raw: res[1] || '' };
    });
}

function buildInstanceBlock(inst) {
    var st = null;
    try { if (inst.raw) st = JSON.parse(inst.raw); } catch (e) {}

    var wrap = E('div', { 'class': 'cbi-section', 'style': 'margin-bottom:1em' },
        [ E('h3', {}, _('Antilag') + ' – ' + inst.sid) ]);

    if (inst.exists && st && st.state) {
        var parts = [ buildStatsTable(st) ];

        if (st.cake_dl && st.cake_dl.tin_cnt)
            parts.push(buildTinTable(_('Download') + ' (' + (st.dl_if || '') + ')', st.cake_dl));
        if (st.cake_ul && st.cake_ul.tin_cnt)
            parts.push(buildTinTable(_('Upload') + ' (' + (st.ul_if || '') + ')', st.cake_ul));

        if (parts.length === 1)
            parts.push(E('div', {
                'style': 'color:#555;margin-top:0.5em'
            }, _('CAKE per-tin statistics unavailable (qdisc not up).')));

        for (var i = 0; i < parts.length; i++)
            wrap.appendChild(parts[i]);
    }
    else if (inst.exists)
        wrap.appendChild(buildSimpleTable(_('Active'), '#1a7f1a'));
    else if (instanceEnabled(inst.sid))
        wrap.appendChild(buildSimpleTable(_('Enabled – not running'), '#c07700'));
    else
        wrap.appendChild(buildSimpleTable(_('Disabled'), '#888'));

    return wrap;
}

/* ── Poller ──────────────────────────────────────────────────── */

var POLL_MS = 3000;

function startPoller(container) {
    function poll() {
        uci.load('antilag').then(function() {
            var sections = uci.sections('antilag', 'antilag');
            var sids = [];
            for (var i = 0; i < sections.length; i++)
                sids.push(sections[i]['.name']);

            return Promise.all(sids.map(fetchInstance)).then(function(results) {
                while (container.firstChild)
                    container.removeChild(container.firstChild);

                if (!results.length)
                    container.appendChild(E('div', { 'class': 'cbi-section' },
                        E('p', {}, _('No antilag instances configured.'))));

                results.forEach(function(inst) {
                    container.appendChild(buildInstanceBlock(inst));
                });
            });
        });
    }

    poll();
    return setInterval(poll, POLL_MS);
}

return baseclass.extend({
    /*
     * render(container) – populate <container> with the per-instance
     * status blocks and start the poll loop.  Polling stops
     * automatically once the container is removed from the DOM.
     */
    render: function(container) {
        var pollInterval = startPoller(container);

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
