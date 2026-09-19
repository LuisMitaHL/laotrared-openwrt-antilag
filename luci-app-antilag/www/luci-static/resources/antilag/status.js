'use strict';
'require baseclass';
'require rpc';
'require uci';

/*
 * antilag/status.js  –  shared Antilag live-status renderer.
 *
 * Used by both the LuCI Overview widget (status/include/75_antilag.js)
 * and the Antilag settings page.  render(container, options) fills
 * <container> with one status block per UCI 'antilag' section and polls
 * every POLL_MS.  A MutationObserver stops the poller when the container
 * leaves the DOM (LuCI swaps status includes / page nodes).
 *
 * The per-tin CAKE tables are only available on the Antilag settings
 * page: `options.tinToggle` renders the checkbox controlling them and
 * the choice is remembered per browser in localStorage.  The Overview
 * widget passes no options and therefore always renders the basic
 * status data only.
 *
 * Multi-instance: every UCI 'antilag' section runs its own daemon
 * process with its own status file (/var/run/antilag-<section>.json).
 * Static-mode sections have no daemon; their status file is written once
 * by `antilag apply` and rendered as a compact fixed-rate block.
 *
 * Per-instance state logic (in priority order):
 *   1. status file exists and parses   → stats (+ optional tin tables) (dynamic)
 *                                        or compact block (static)
 *   2. file exists but content invalid → red parse-error row (never silent)
 *   3. file exists but unreadable      → red unreadable row (never silent)
 *   4. UCI enabled = 1 but no file     → enabled but not started / crashed
 *   5. UCI enabled = 0                 → disabled
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

/*
 * ── CAKE tin statistics visibility ──────────────────────────────
 *
 * The per-tin tables are large, so they are hidden by default.  The
 * preference is a per-browser display setting (not a UCI option):
 * it is toggled by the checkbox shown only on the Services → Antilag
 * page and persists in localStorage across page loads.
 */
var TIN_PREF_KEY = 'antilag.showCakeTins';

function showCakeTins() {
    try { return window.localStorage.getItem(TIN_PREF_KEY) === '1'; }
    catch (e) { return false; }
}

function setShowCakeTins(on) {
    try { window.localStorage.setItem(TIN_PREF_KEY, on ? '1' : '0'); }
    catch (e) { /* private mode / storage disabled – keep in-memory default */ }
}

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
        return E('td', { 'class': 'td', 'style': 'color:var(--error-color-medium,#c00);font-weight:bold' },
                 _('Bufferbloat'));
    var map = {
        high:    [ 'color:var(--warn-color-medium,#c07700);font-weight:bold', _('▲ High')   ],
        low:     [ 'color:var(--text-color-low,#888)',                        _('▼ Low')    ],
        running: [ 'color:var(--success-color-medium,#1a7f1a)',               _('● Normal') ]
    };
    var e = map[load] || [ '', _('— Idle') ];
    return E('td', { 'class': 'td', 'style': e[0] }, e[1]);
}

/* ── Table builders ──────────────────────────────────────────── */

function buildStatsTable(st) {
    var stateColors = { running: 'var(--success-color-medium,#1a7f1a)', idle: 'var(--text-color-low,#888)', stall: 'var(--error-color-medium,#c00)' };
    var color = stateColors[st.state] || 'var(--text-color-low,#888)';
    var stateLabels = { running: _('Running'), idle: _('Idle'), stall: _('Stall') };
    var label = stateLabels[st.state] || '?';
    var owdDlStyle = (st.avg_owd_dl_ms10 > 100) ? 'color:var(--error-color-medium,#c00)' : 'color:var(--text-color-medium,#555)';
    var owdUlStyle = (st.avg_owd_ul_ms10 > 100) ? 'color:var(--error-color-medium,#c00)' : 'color:var(--text-color-medium,#555)';

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
            E('td', { 'class': 'td', 'style': 'color:var(--text-color-medium,#555)' },       fmtKbps(st.achieved_dl_kbps)),
            loadCell(st.load_dl, st.bb_dl),
            E('td', { 'class': 'td', 'style': owdDlStyle },         fmtOwd(st.avg_owd_dl_ms10)),
            E('td', { 'class': 'td', 'style': 'font-weight:bold' }, fmtKbps(st.shaper_ul_kbps)),
            E('td', { 'class': 'td', 'style': 'color:var(--text-color-medium,#555)' },       fmtKbps(st.achieved_ul_kbps)),
            loadCell(st.load_ul, st.bb_ul),
            E('td', { 'class': 'td', 'style': owdUlStyle },         fmtOwd(st.avg_owd_ul_ms10)),
            E('td', { 'class': 'td', 'style': 'color:var(--text-color-medium,#555)' },       fmtUptime(st.uptime_s))
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
                'style': t.dropped_packets ? 'color:var(--error-color-medium,#c00)' : 'color:var(--text-color-medium,#555)'
            }, t.dropped_packets + ' / ' + fmtBytes(t.dropped_bytes)),
            E('td', { 'class': 'td' },
                t.ecn_packets ? fmtPkts(t.ecn_packets) : '—'),
            E('td', { 'class': 'td' }, fmtBytes(t.backlog_bytes)),
            E('td', { 'class': 'td', 'style': 'color:var(--text-color-medium,#555)' }, fmtUs(t.avg_delay_us))
        ]));
    }

    var header = [ E('h3', {}, _('CAKE tin stats') + ' – ' + title + ' (' + qd.kind + ')') ];
    var meta   = [];
    if (qd.capacity_bps)
        meta.push(_('Capacity estimate') + ': ' + fmtBps(qd.capacity_bps));
    if (qd.memory_limit)
        meta.push(_('Memory') + ': ' + fmtBytes(qd.memory_used) + ' / ' + fmtBytes(qd.memory_limit));
    if (meta.length)
        header.push(E('div', { 'style': 'color:var(--text-color-medium,#555);margin-bottom:0.4em' }, meta.join(' · ')));

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

/*
 * buildStaticTable – compact block for a static (fixed-rate) instance.
 * There is no daemon, so there are no live metrics: show the installed
 * shaper rates and which directions are actually shaped.
 */
function buildStaticTable(st) {
    function rate(active, kbps) {
        return active ? fmtKbps(kbps) : '—';
    }

    return E('table', { 'class': 'table', 'id': 'antilag_status_table' }, [
        E('tr', { 'class': 'tr table-titles' }, [
            E('th', { 'class': 'th' }, _('Status')),
            E('th', { 'class': 'th' }, _('DL Shaped')),
            E('th', { 'class': 'th' }, _('UL Shaped')),
            E('th', { 'class': 'th' }, _('Interfaces'))
        ]),
        E('tr', { 'class': 'tr' }, [
            E('td', {
                'class': 'td',
                'style': 'font-weight:bold;color:var(--success-color-medium,#1a7f1a)'
            }, _('Static')),
            E('td', { 'class': 'td', 'style': 'font-weight:bold' },
                rate(st.dl_active, st.shaper_dl_kbps)),
            E('td', { 'class': 'td', 'style': 'font-weight:bold' },
                rate(st.ul_active, st.shaper_ul_kbps)),
            E('td', { 'class': 'td', 'style': 'color:var(--text-color-medium,#555)' },
                (st.dl_if || '—') + ' / ' + (st.ul_if || '—'))
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

/*
 * decodePayload – normalize whatever `file read` resolved to into text.
 * Shape varies by rpcd/LuCI version: raw file text (current rpc.js
 * unwraps expect:{data} to the string), the full {data:...} object
 * (no unwrap), or base64-encoded text (older rpcd). Raw JSON never
 * matches the base64 alphabet ({, ", : are outside it), so the
 * probe below only fires on real base64.
 */
function decodePayload(v) {
    if (v == null)
        return '';
    if (typeof v === 'object')
        v = v.data || '';
    if (typeof v !== 'string')
        return '';
    var s = v.replace(/\s+/g, '');
    if (s.length >= 8 && (s.length % 4) === 0 &&
        /^[A-Za-z0-9+/]+={0,2}$/.test(s)) {
        try {
            var bin = atob(s);
            var bytes = new Uint8Array(bin.length);
            for (var i = 0; i < bin.length; i++)
                bytes[i] = bin.charCodeAt(i) & 0xff;
            if (typeof TextDecoder !== 'undefined')
                return new TextDecoder().decode(bytes);
            return bin;
        } catch (e) { /* not base64 – fall through to raw */ }
    }
    return v;
}

function fetchInstance(sid) {
    return Promise.all([
        callFileStat(statusPath(sid)).catch(function() { return ''; }),
        callFileRead(statusPath(sid)).catch(function() { return ''; })
    ]).then(function(res) {
        return { sid: sid, exists: !!res[0], raw: decodePayload(res[1]) };
    });
}

function buildInstanceBlock(inst, withTins) {
    var st = null;
    try { if (inst.raw) st = JSON.parse(inst.raw); } catch (e) {}

    var wrap = E('div', { 'class': 'cbi-section', 'style': 'margin-bottom:1em' },
        [ E('h3', {}, _('Antilag') + ' – ' + inst.sid) ]);

    if (inst.exists && st && st.state) {
        var parts = [];

        if (st.mode === 'static') {
            parts.push(buildStaticTable(st));
            for (var s = 0; s < parts.length; s++)
                wrap.appendChild(parts[s]);
            return wrap;
        }

        parts.push(buildStatsTable(st));

        /*
         * The per-tin CAKE tables are opt-in: they dominate the status
         * block, so they are only rendered when the user enabled the
         * "Show CAKE tin statistics" checkbox on the Antilag page.
         */
        if (withTins) {
            if (st.cake_dl && st.cake_dl.tin_cnt)
                parts.push(buildTinTable(_('Download') + ' (' + (st.dl_if || '') + ')', st.cake_dl));
            if (st.cake_ul && st.cake_ul.tin_cnt)
                parts.push(buildTinTable(_('Upload') + ' (' + (st.ul_if || '') + ')', st.cake_ul));

            if (parts.length === 1)
                parts.push(E('div', {
                    'style': 'color:var(--text-color-medium,#555);margin-top:0.5em'
                }, _('CAKE per-tin statistics unavailable (qdisc not up).')));
        }

        for (var i = 0; i < parts.length; i++)
            wrap.appendChild(parts[i]);
    }
    else if (inst.exists && inst.raw)
        wrap.appendChild(buildSimpleTable(_('Running – status parse error'), 'var(--error-color-medium,#c00)'));
    else if (inst.exists)
        wrap.appendChild(buildSimpleTable(_('Running – status unreadable'), 'var(--error-color-medium,#c00)'));
    else if (instanceEnabled(inst.sid))
        wrap.appendChild(buildSimpleTable(
            uci.get('antilag', inst.sid, 'mode') === 'static'
                ? _('Enabled – not applied (waiting for interface)')
                : _('Enabled – not running'),
            'var(--warn-color-medium,#c07700)'));
    else
        wrap.appendChild(buildSimpleTable(_('Disabled'), 'var(--text-color-low,#888)'));

    return wrap;
}

/* ── Poller ──────────────────────────────────────────────────── */

var POLL_MS = 3000;

function startPoller(container, allowTins) {
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

                var withTins = allowTins && showCakeTins();
                results.forEach(function(inst) {
                    container.appendChild(buildInstanceBlock(inst, withTins));
                });
            });
        });
    }

    poll();

    return {
        interval: setInterval(poll, POLL_MS),
        refresh:  poll
    };
}

return baseclass.extend({
    /*
     * render(container, options) – populate <container> with the
     * per-instance status blocks and start the poll loop.  Polling
     * stops automatically once the container is removed from the DOM.
     *
     * options.tinToggle – when true, the per-tin CAKE tables may be
     * shown and the "Show CAKE tin statistics" checkbox is rendered.
     * Only the Antilag settings page opts in; the Overview widget
     * always shows the basic status data only.
     */
    render: function(container, options) {
        options = options || {};

        var poller, content = container;

        if (options.tinToggle) {
            var toggle = E('input', {
                'type': 'checkbox',
                'id': 'antilag-cake-tins-toggle',
                'style': 'margin:0 0.5em 0 0;vertical-align:middle'
            });
            toggle.checked = showCakeTins();
            toggle.addEventListener('change', function(ev) {
                setShowCakeTins(ev.currentTarget.checked);
                if (poller)
                    poller.refresh();
            });

            container.appendChild(E('div', {
                'class': 'cbi-section',
                'style': 'margin-bottom:1em'
            }, [
                E('label', {
                    'for': 'antilag-cake-tins-toggle',
                    'style': 'cursor:pointer'
                }, [
                    toggle,
                    E('strong', {}, _('Show CAKE tin statistics'))
                ]),
                E('div', {
                    'class': 'cbi-section-descr'
                }, _('Display the live per-tin CAKE qdisc counters (threshold, ' +
                     'sent, dropped, ECN marks, backlog and average delay) ' +
                     'below each instance. Hidden by default.'))
            ]));

            content = E('div', {});
            container.appendChild(content);
        }

        poller = startPoller(content, !!options.tinToggle);

        var observer = new MutationObserver(function(mutations) {
            mutations.forEach(function(m) {
                m.removedNodes.forEach(function(node) {
                    if (node === container ||
                        (node.contains && node.contains(container))) {
                        clearInterval(poller.interval);
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
