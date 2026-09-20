'use strict';
'require view';
'require form';
'require uci';
'require ui';
'require rpc';
'require tools.widgets as widgets';
'require antilag.status as antilagStatus';

var callInitAction = rpc.declare({
    object: 'luci',
    method: 'setInitAction',
    params: [ 'name', 'action' ],
    expect: { result: false }
});

/*
 * makeServiceButton – returns a <button> DOM node that calls callInitAction,
 * disables itself while pending, and shows a success/error notification.
 */
function makeServiceButton(label, style, action) {
    return E('button', {
        'class': 'btn cbi-button cbi-button-' + style,
        'click': function(ev) {
            var btn = ev.currentTarget;
            btn.disabled = true;
            return callInitAction('antilag', action)
                .then(function(res) {
                    btn.disabled = false;
                    ui.addNotification(null,
                        E('p', res
                            ? _('Service %s successful.').format(action)
                            : _('Service %s failed.').format(action)),
                        res ? 'info' : 'error');
                })
                .catch(function(err) {
                    btn.disabled = false;
                    ui.addNotification(null,
                        E('p', _('RPC error: %s').format(err.message || String(err))),
                        'error');
                });
        }
    }, label);
}

return view.extend({
    load: function() {
        return uci.load('antilag');
    },

    /*
     * handleSaveApply – override the default Save & Apply so that after
     * UCI changes are committed we always restart the daemon.
     *
     * procd's reload-trigger mechanism only fires on already-registered
     * (running) service instances.  If the service was disabled (no running
     * instance) and the user enables it and hits Save & Apply, procd has
     * nothing to trigger and the daemon never starts.  Explicitly calling
     * restart here covers both the cold-start (was disabled) and the
     * already-running (settings change) cases, for every instance.
     */
    handleSaveApply: function(ev, mode) {
        return this.handleSave(ev).then(function() {
            return ui.changes.apply(mode).then(function() {
                return callInitAction('antilag', 'restart')
                    .catch(function() { /* ignore if service not present yet */ });
            });
        });
    },

    render: function() {
        var m, s, o;

        m = new form.Map('antilag', _('Antilag'),
            _('Adaptive CAKE shaper – automatically adjusts download and upload ' +
              'bandwidth based on measured one-way delay (OWD). ' +
              'Standalone operation: no sqm-scripts required. ' +
              'Each instance below shapes one WAN pair – add one instance per WAN ' +
              'for multi-WAN setups (e.g. with mwan3).<br>' +
              'Set <strong>Mode</strong> to <strong>Static</strong> for a fixed ' +
              'target rate with no reflectors and no live changes (the ' +
              'sqm-scripts model).'));

        /* ── Instances grid ────────────────────────────────────
         *
         * One row per UCI 'antilag' section = one WAN shaping instance.
         * The grid shows a summary; "More Options…" opens the full
         * editor with all settings, grouped in tabs.
         */
        s = m.section(form.GridSection, 'antilag', _('Instances'));
        s.addremove = true;
        s.anonymous = false;
        s.editable = true;
        s.nodescriptions = true;
        s.modaltitle = function(section_id) {
            return _('Antilag instance') + ': ' + section_id;
        };

        /* Tabs for the edit modal */
        s.tab('general',  _('General'));
        s.tab('qdisc',    _('CAKE Qdisc'));
        s.tab('advanced', _('Advanced'));
        s.tab('health',   _('Reflector Health'));

        function mopt(tab, type, name, label, description) {
            var opt = s.taboption(tab, type, name, label, description);
            opt.modalonly = true;
            return opt;
        }

        /*
         * moptDyn – a modal option that only applies to adaptive (dynamic)
         * instances.  Static instances have no reflectors and never adjust
         * the shaper, so their tuning options are hidden when mode=static.
         */
        function moptDyn(tab, type, name, label, description) {
            var opt = mopt(tab, type, name, label, description);
            opt.depends('mode', 'dynamic');
            return opt;
        }

        /* ── Grid summary columns ──────────────────────────────
         *
         * These core options are deliberately left at the default
         * `modalonly = null`, which makes LuCI render them as read-only
         * text previews in the grid table AND as editable widgets in the
         * instance modal.  It is essential that `mode` is part of the
         * modal: the dynamic-only options below use `depends('mode',
         * 'dynamic')`, and LuCI resolves dependencies within the modal's
         * own form map.  Marking these `modalonly = false` would remove
         * them from the modal and silently hide every dependent option.
         */
        o = s.taboption('general', form.Flag, 'enabled', _('Enable'),
            _('Master switch for this instance. When off, the daemon does not ' +
              'start and no CAKE qdisc is installed for this WAN pair.'));
        o.default = '0';
        o.rmempty = false;

        o = s.taboption('general', form.ListValue, 'mode', _('Mode'),
            _('<strong>Dynamic</strong> (default): continuously measures latency ' +
              'against reflectors and adapts the download/upload shaper rates. ' +
              'Runs as a daemon.<br>' +
              '<strong>Static</strong>: applies a fixed target download/upload ' +
              'rate exactly like sqm-scripts — no reflectors, no live changes, ' +
              'no daemon. The interface hotplug hook re-applies it whenever the ' +
              'WAN interface comes back up. Use this if you only want a fixed ' +
              'CAKE shaper or need to avoid conflicting with another QoS tool.'));
        o.default = 'dynamic';
        o.value('dynamic', _('Dynamic – adaptive CAKE (OWD-driven)'));
        o.value('static',  _('Static – fixed target rate (sqm-scripts style)'));
        o.rmempty = false;

        /*
         * ul_if – WAN-facing egress interface, picked from LuCI's device
         * list (the same widget sqm-scripts uses).  noaliases drops the
         * logical-network aliases (@wan, …) because the daemon resolves
         * this name against the kernel, and existing IFBs are filtered out
         * since an IFB can never be the WAN interface.
         */
        o = s.taboption('general', widgets.DeviceSelect, 'ul_if',
            _('Upload Interface'),
            _('WAN-facing interface for egress shaping, chosen from the ' +
              'available devices (e.g. <code>wan</code>). ' +
              'Must be unique per instance. The download IFB interface is ' +
              'generated from this value as <code>ifb-&lt;interface&gt;</code>.'));
        o.rmempty = false;
        o.noaliases = true;
        o.filter = function(section_id, value) {
            return !/^ifb/.test(value);
        };
        /* Keep the derived dl_if field in sync while the modal is open.
         * The lookup is scoped to this option's own form map so it always
         * finds the visible modal's field, never a hidden stacked one. */
        o.onchange = function(ev, section_id, value) {
            var el = this.map.findElement('id',
                'widget.cbid.%s.%s.dl_if'.format(this.map.config, section_id));
            if (el)
                el.value = value ? 'ifb-' + value : '';
        };

        o = s.taboption('general', form.Value, 'base_dl_shaper_rate_kbps',
            _('Base Download Rate (kbps)'),
            _('Steady-state download rate held when the link is idle or lightly ' +
              'loaded, and the value the algorithm decays back to. Set it to your ' +
              'measured download speed minus roughly 5–10%. The rate ramps up from ' +
              'here under load and backs off when latency rises. Rate accounting is ' +
              'gross (it includes framing overhead), so a speed test reads a little ' +
              'lower than the value entered here.<br>' +
              'In <strong>Static</strong> mode this is simply the fixed download ' +
              'shaping target.'));
        o.datatype = 'uinteger';
        o.default  = '20000';

        o = s.taboption('general', form.Value, 'base_ul_shaper_rate_kbps',
            _('Base Upload Rate (kbps)'),
            _('Steady-state upload rate held when the link is idle or lightly ' +
              'loaded, and the value the algorithm decays back to. Set it to your ' +
              'measured upload speed minus roughly 5–10%. The rate ramps up from ' +
              'here under load and backs off when latency rises. Rate accounting is ' +
              'gross (it includes framing overhead), so a speed test reads a little ' +
              'lower than the value entered here.<br>' +
              'In <strong>Static</strong> mode this is simply the fixed upload ' +
              'shaping target.'));
        o.datatype = 'uinteger';
        o.default  = '20000';

        /* ════════════════════════════════════════════════════
         * General tab (modal)
         * ════════════════════════════════════════════════════ */
        /*
         * dl_if – IFB interface, always generated from ul_if as
         * ifb-<ul_if> (the sqm-scripts model: e.g. ul_if "wan" gives
         * "ifb-wan").  The field is read-only; both cfgvalue() and
         * formvalue() derive the name so a stale value in UCI is corrected
         * on the next save, and forcewrite ensures it is persisted even
         * when nothing else changed.
         */
        o = mopt('general', form.Value, 'dl_if',
            _('Download Interface'),
            _('IFB interface that carries shaped ingress traffic ' +
              '(<code>ifb-wan</code> when the upload interface is ' +
              '<code>wan</code>). Generated automatically from the Upload ' +
              'Interface and created by the daemon at startup if it does ' +
              'not exist.'));
        o.readonly = true;
        o.forcewrite = true;
        o.rmempty = false;
        o.cfgvalue = function(section_id) {
            var ul = uci.get('antilag', section_id, 'ul_if');
            return ul ? 'ifb-' + ul : null;
        };
        o.formvalue = function(section_id) {
            return this.cfgvalue(section_id);
        };

        o = moptDyn('general', form.Flag, 'adjust_dl_shaper_rate', _('Adjust Download Shaper'),
            _('Allow the daemon to actively change the download shaper rate. ' +
              'Disable to hold the download qdisc at its base rate and only ' +
              'measure (for example when another tool owns download shaping).'));
        o.default = '1';

        o = moptDyn('general', form.Flag, 'adjust_ul_shaper_rate', _('Adjust Upload Shaper'),
            _('Allow the daemon to actively change the upload shaper rate. ' +
              'Disable to hold the upload qdisc at its base rate and only measure.'));
        o.default = '1';

        /*
         * ping_bind_if – checkbox backed by the string UCI option.
         * Checked = bind ICMP socket to the Upload Interface (ul_if);
         * unchecked = unbound, follow routing table (single-WAN).
         * cfgvalue maps non-empty string > '1'; write() stores the
         * current ul_if (normalizing legacy values) or deletes the
         * option when unchecked, so the daemon ('SO_BINDTODEVICE when
         * non-empty') needs no change. Never store '0': any non-empty
         * value would bind to a bogus device.
         */
        o = moptDyn('general', form.Flag, 'ping_bind_if', _('Ping Bind Interface'),
            _('Bind the ICMP measurement socket to the Upload Interface ' +
              '(same value as above). ' +
              'Required for multi-WAN: without it, policy routing (mwan3) may send ' +
              'reflector pings out an arbitrary WAN and corrupt the per-WAN measurement. ' +
              'Leave unchecked to follow the routing table (single-WAN).'));
        o.default = '0';
        o.rmempty = true;
        o.cfgvalue = function(section_id) {
            var v = uci.get('antilag', section_id, 'ping_bind_if');
            return (v && v.length > 0) ? '1' : '0';
        };
        o.write = function(section_id, value) {
            if (value !== '1')
                return uci.unset('antilag', section_id, 'ping_bind_if');
            var ul = uci.get('antilag', section_id, 'ul_if');
            if (!ul) {
                var el = this.map.findElement('id',
                    'widget.cbid.%s.%s.ul_if'.format(this.map.config, section_id));
                if (el && el.value)
                    ul = el.value;
            }
            if (ul)
                return uci.set('antilag', section_id, 'ping_bind_if', ul);
            return uci.unset('antilag', section_id, 'ping_bind_if');
        };
        o.remove = function(section_id) {
            return uci.unset('antilag', section_id, 'ping_bind_if');
        };

        function rateOption(tab, name, label, description, def) {
            var opt = moptDyn(tab, form.Value, name, label, description);
            opt.datatype = 'uinteger';
            opt.default  = String(def);
            return opt;
        }

        rateOption('general', 'min_dl_shaper_rate_kbps', _('Min Download Rate (kbps)'),
            _('Lower hard floor for the download rate. The shaper is never reduced ' +
              'below this, even under sustained bufferbloat. Set it to the lowest ' +
              'bufferbloat-free download rate you have observed.'), 5000);
        rateOption('general', 'max_dl_shaper_rate_kbps', _('Max Download Rate (kbps)'),
            _('Upper ceiling for the download rate. Under high load the algorithm ' +
              'ramps up toward this and backs off when latency rises. Set it to the ' +
              'maximum your link can deliver, or slightly below to trade a little ' +
              'throughput for tighter latency.'), 80000);
        rateOption('general', 'min_ul_shaper_rate_kbps', _('Min Upload Rate (kbps)'),
            _('Lower hard floor for the upload rate. The shaper is never reduced ' +
              'below this, even under sustained bufferbloat. Set it to the lowest ' +
              'bufferbloat-free upload rate you have observed.'), 5000);
        rateOption('general', 'max_ul_shaper_rate_kbps', _('Max Upload Rate (kbps)'),
            _('Upper ceiling for the upload rate. Under high load the algorithm ' +
              'ramps up toward this and backs off when latency rises. Set it to the ' +
              'maximum your link can deliver, or slightly below to keep latency ' +
              'tighter.'), 35000);
        rateOption('general', 'connection_active_thr_kbps',
            _('Connection Active Threshold (kbps)'),
            _('Achieved-rate threshold below which a direction counts as idle. ' +
              'Below it the rate decays back toward base and the sleep function ' +
              'may pause pinging.'), 2000);

        /* ════════════════════════════════════════════════════
         * CAKE Qdisc tab (modal)
         * ════════════════════════════════════════════════════ */

        o = mopt('qdisc', form.ListValue, 'cake_diffserv',
            _('Traffic Classification'),
            _('Determines how packets are sorted into CAKE tins for prioritisation.'));
        o.default = '1';
        o.value('1', _('diffserv4 – 4 tins: Bulk / Best-Effort / Streaming / Voice (recommended)'));
        o.value('2', _('diffserv8 – 8 tins mapped to DSCP CS0–CS7'));
        o.value('3', _('besteffort – single tin, no prioritisation'));
        o.value('4', _('precedence – legacy IP Precedence field'));

        o = mopt('qdisc', form.ListValue, 'cake_flow_mode',
            _('Flow Isolation (shared default)'),
            _('Fallback isolation mode used for both DL and UL when the ' +
              'direction-specific options below are not set.'));
        o.default = '7';
        o.value('7', _('triple – src host + dst host + 5-tuple flow (recommended)'));
        o.value('5', _('dual-srchost – per src host + flow'));
        o.value('6', _('dual-dsthost – per dst host + flow'));
        o.value('3', _('hosts – per src+dst host pair'));
        o.value('4', _('flows – per 5-tuple flow only'));
        o.value('0', _('none – no isolation'));

        o = mopt('qdisc', form.ListValue, 'cake_dl_flow_mode',
            _('Flow Isolation – Download (IFB/ingress)'),
            _('Isolation for the IFB ingress qdisc. ' +
              'For asymmetric links, <strong>dual-dsthost</strong> is often better: ' +
              'it isolates per destination (local host), preventing any single ' +
              'LAN client from starving others on the download side. ' +
              'Leave at "-1 (use shared default)" to inherit from the setting above.'));
        o.default = '-1';
        o.value('-1', _('− use shared default above −'));
        o.value('7', _('triple – src host + dst host + 5-tuple flow'));
        o.value('6', _('dual-dsthost – per dst host + flow (recommended for DL)'));
        o.value('5', _('dual-srchost – per src host + flow'));
        o.value('3', _('hosts – per src+dst host pair'));
        o.value('4', _('flows – per 5-tuple flow only'));
        o.value('0', _('none – no isolation'));

        o = mopt('qdisc', form.ListValue, 'cake_ul_flow_mode',
            _('Flow Isolation – Upload (WAN/egress)'),
            _('Isolation for the WAN egress qdisc. ' +
              'For asymmetric links, <strong>dual-srchost</strong> is often better: ' +
              'it isolates per source (local host), preventing any single ' +
              'LAN client from monopolising upload capacity. ' +
              'Leave at "-1 (use shared default)" to inherit from the setting above.'));
        o.default = '-1';
        o.value('-1', _('− use shared default above −'));
        o.value('7', _('triple – src host + dst host + 5-tuple flow'));
        o.value('5', _('dual-srchost – per src host + flow (recommended for UL)'));
        o.value('6', _('dual-dsthost – per dst host + flow'));
        o.value('3', _('hosts – per src+dst host pair'));
        o.value('4', _('flows – per 5-tuple flow only'));
        o.value('0', _('none – no isolation'));

        o = mopt('qdisc', form.Flag, 'cake_nat',
            _('NAT-Aware Flow Hashing'),
            _('Peek inside NAT/masquerade to correctly identify flows by their real ' +
              'source address. Strongly recommended for home routers doing SNAT.'));
        o.default = '1';

        o = mopt('qdisc', form.Flag, 'cake_wash',
            _('DSCP Wash (Upload)'),
            _('Strip DSCP markings on egress (upload to ISP) so the carrier cannot ' +
              'exploit your internal QoS markings. Applied to the WAN qdisc only; ' +
              'download markings are preserved for local use.'));
        o.default = '1';

        o = mopt('qdisc', form.Value, 'cake_overhead',
            _('Per-Packet Overhead (bytes)'),
            _('Framing overhead added to each packet before rate accounting. ' +
              'Common values: <code>0</code> = Ethernet/plain, ' +
              '<code>8</code> = PPPoE over PTM (VDSL2), ' +
              '<code>18</code> = PPPoE over ATM (ADSL LLC/SNAP). ' +
              'Being close matters more than being exact: underestimating lets too ' +
              'much traffic into the link and causes bufferbloat, while ' +
              'overestimating by a few bytes only costs a little throughput, so ' +
              'when in doubt err on the high side.'));
        o.datatype = 'integer';
        o.default  = '0';
        o.placeholder = '0';

        o = mopt('qdisc', form.ListValue, 'cake_atm',
            _('ATM/PTM Cell Compensation'),
            _('Hardware-layer cell-size rounding. Use instead of (or in addition to) overhead ' +
              'for DSL links with cell-based framing.'));
        o.default = '0';
        o.value('0', _('None (default – Ethernet / PTM with overhead)'));
        o.value('1', _('ATM – 48-byte cell rounding (ADSL)'));
        o.value('2', _('PTM – 64-byte expansion (VDSL2 alternative)'));

        o = mopt('qdisc', form.Value, 'cake_mpu',
            _('Minimum Packet Unit (bytes)'),
            _('Packets shorter than this are padded before rate accounting. ' +
              '<code>0</code> = disabled (CAKE default). Accounting for very small ' +
              'packets matters on highly asymmetric links, where pure TCP ACKs can ' +
              'fill the upload; <code>64</code> is a common safe value for ' +
              'Ethernet-based links.'));
        o.datatype = 'uinteger';
        o.default  = '0';
        o.placeholder = '0';

        o = mopt('qdisc', form.ListValue, 'cake_ack_filter',
            _('ACK Filter'),
            _('Suppress redundant TCP ACKs on the upload path to recover capacity ' +
              'on asymmetric connections. Off by default (safest).'));
        o.default = '0';
        o.value('0', _('Off (default)'));
        o.value('1', _('Moderate filtering'));
        o.value('2', _('Aggressive'));

        o = mopt('qdisc', form.Value, 'cake_rtt_ms',
            _('Target RTT (ms)'),
            _("CAKE's internal AQM target RTT. " +
              '<code>0</code> = use CAKE default (100 ms). ' +
              'Lower values improve latency on very fast links; ' +
              'higher values suit high-latency paths.'));
        o.datatype = 'uinteger';
        o.default  = '0';
        o.placeholder = '0 (CAKE default = 100 ms)';

        o = mopt('qdisc', form.Flag, 'cake_split_gso',
            _('Split GSO Super-Packets'),
            _('Break large GSO/GRO segment groups into individual packets before scheduling. ' +
              'Strongly recommended – improves per-packet latency on high-throughput links.'));
        o.default = '1';

        /* ════════════════════════════════════════════════════
         * Advanced tab (modal)
         * ════════════════════════════════════════════════════ */

        /* ── Pinger mode ──────────────────────────────── */
        o = moptDyn('advanced', form.ListValue, 'ping_type',
            _('Ping Type'),
            _('ICMP packet type used for OWD measurement.<br>' +
              '<strong>ICMP Echo (type 8)</strong>: measures RTT; OWD = RTT/2. ' +
              'Works against any host. Assumes symmetric path — inaccurate ' +
              'for asymmetric connections (DSL, cable, DOCSIS).<br>' +
              '<strong>ICMP Timestamp (type 13)</strong>: reflector returns its own ' +
              'receive/transmit timestamps giving true per-direction OWD. ' +
              'Recommended for asymmetric broadband. Requires reflectors that ' +
              'support timestamp replies — see ' +
              '<a href="https://github.com/tievolu/timestamp-reflectors" target="_blank">' +
              'timestamp-reflectors</a>.'));
        o.default = '0';
        o.value('0', _('ICMP Echo (type 8) – RTT/2, symmetric estimate'));
        o.value('1', _('ICMP Timestamp (type 13) – true per-direction OWD'));

        o = moptDyn('advanced', form.Value, 'reflectors_file',
            _('Reflectors File'),
            _('Path to a plain-text file of reflector IP addresses, one per line ' +
              '(lines starting with <code>#</code> are ignored). ' +
              'When set, overrides the Reflectors list above. ' +
              'The daemon shuffles the list on each startup for reflector diversity.<br>' +
              'Intended for use with large timestamp-reflector lists from ' +
              '<a href="https://github.com/tievolu/timestamp-reflectors" target="_blank">' +
              'github.com/tievolu/timestamp-reflectors</a>.<br>' +
              'Leave blank to use the Reflectors list.'));
        o.placeholder = '/etc/antilag/timestamp-reflectors.txt';

        /* ── Pinger tuning ────────────────────────────── */
        o = moptDyn('advanced', form.Value, 'no_pingers',
            _('Number of Active Pingers'),
            _('How many reflectors to ping concurrently.'));
        o.datatype = 'range(1,20)';
        o.default  = '6';

        o = moptDyn('advanced', form.Value, 'reflector_ping_interval_s',
            _('Ping Interval (s)'),
            _('Time between pings to each individual reflector.'));
        o.datatype = 'float';
        o.default  = '0.3';

        o = moptDyn('advanced', form.DynamicList, 'reflectors',
            _('Reflectors'),
            _('ICMP ping targets. The first <em>N</em> (= Number of Active Pingers) ' +
              'are active; the rest are spares for automatic replacement.'));
        o.datatype = 'or(ipaddr, hostname)';

        o = moptDyn('advanced', form.Value, 'dl_owd_delta_delay_thr_ms',
            _('DL Delay Threshold (ms)'),
            _('OWD delta above this counts as a bufferbloat event for download.'));
        o.datatype = 'float'; o.default = '30.0';

        o = moptDyn('advanced', form.Value, 'ul_owd_delta_delay_thr_ms',
            _('UL Delay Threshold (ms)'),
            _('OWD delta above this counts as a bufferbloat event for upload. ' +
              'The upload counterpart of the DL delay threshold.'));
        o.datatype = 'float'; o.default = '30.0';

        o = moptDyn('advanced', form.Value, 'dl_avg_owd_delta_max_adjust_up_thr_ms',
            _('DL Avg OWD Max Adjust-Up Threshold (ms)'),
            _('At or below this average OWD delta, rate increases at maximum speed.'));
        o.datatype = 'float'; o.default = '10.0';

        o = moptDyn('advanced', form.Value, 'ul_avg_owd_delta_max_adjust_up_thr_ms',
            _('UL Avg OWD Max Adjust-Up Threshold (ms)'),
            _('Upload counterpart of the adjust-up threshold: at or below this ' +
              'average OWD delta, the upload rate increases at maximum speed.'));
        o.datatype = 'float'; o.default = '10.0';

        o = moptDyn('advanced', form.Value, 'dl_avg_owd_delta_max_adjust_down_thr_ms',
            _('DL Avg OWD Max Adjust-Down Threshold (ms)'),
            _('At or above this average OWD delta, rate decreases at maximum speed.'));
        o.datatype = 'float'; o.default = '60.0';

        o = moptDyn('advanced', form.Value, 'ul_avg_owd_delta_max_adjust_down_thr_ms',
            _('UL Avg OWD Max Adjust-Down Threshold (ms)'),
            _('Upload counterpart of the adjust-down threshold: at or above this ' +
              'average OWD delta, the upload rate is reduced at maximum severity.'));
        o.datatype = 'float'; o.default = '60.0';

        o = moptDyn('advanced', form.Value, 'alpha_baseline_increase',
            _('Baseline EWMA α (increase)'),
            _('Small value → slow upward tracking (0.001 = very slow).'));
        o.datatype = 'float'; o.default = '0.001';

        o = moptDyn('advanced', form.Value, 'alpha_baseline_decrease',
            _('Baseline EWMA α (decrease)'),
            _('Large value → fast relaxation when OWD drops (0.9 = fast).'));
        o.datatype = 'float'; o.default = '0.9';

        o = moptDyn('advanced', form.Value, 'alpha_delta_ewma',
            _('Delta EWMA α'),
            _('Smoothing factor for the OWD delta above baseline. This smoothed ' +
              'value scales how hard the rate is reduced on bufferbloat. Small = ' +
              'steadier and slower to react; large = reacts faster to spikes.'));
        o.datatype = 'float'; o.default = '0.095';

        o = moptDyn('advanced', form.Value, 'shaper_rate_min_adjust_down_bufferbloat',
            _('Min Rate Down Factor (bufferbloat)'),
            _('Minimum multiplier on bufferbloat (e.g. 0.99 = −1%).'));
        o.datatype = 'float'; o.default = '0.99';

        o = moptDyn('advanced', form.Value, 'shaper_rate_max_adjust_down_bufferbloat',
            _('Max Rate Down Factor (bufferbloat)'),
            _('Maximum multiplier on severe bufferbloat (e.g. 0.75 = −25%).'));
        o.datatype = 'float'; o.default = '0.75';

        o = moptDyn('advanced', form.Value, 'shaper_rate_min_adjust_up_load_high',
            _('Min Rate Up Factor (high load)'),
            _('Smallest multiplier applied when increasing the rate under high ' +
              'load. The increase scales down toward this value as the average ' +
              'OWD delta approaches the delay threshold (1.0 = no increase at ' +
              'the limit).'));
        o.datatype = 'float'; o.default = '1.0';

        o = moptDyn('advanced', form.Value, 'shaper_rate_max_adjust_up_load_high',
            _('Max Rate Up Factor (high load)'),
            _('Largest multiplier applied when increasing the rate under high ' +
              'load (e.g. 1.04 = +4% per adjustment). Applied in full while the ' +
              'average OWD delta is at or below the adjust-up threshold.'));
        o.datatype = 'float'; o.default = '1.04';

        o = moptDyn('advanced', form.Value, 'shaper_rate_adjust_down_load_low',
            _('Rate Down Factor (low load)'),
            _('Multiplier used to step the rate back down toward the base rate ' +
              'when load is low or idle (e.g. 0.99 = −1% per adjustment).'));
        o.datatype = 'float'; o.default = '0.99';

        o = moptDyn('advanced', form.Value, 'shaper_rate_adjust_up_load_low',
            _('Rate Up Factor (low load)'),
            _('Multiplier used to step the rate back up toward the base rate when ' +
              'the current rate is below base and load is low (e.g. 1.01 = +1% ' +
              'per adjustment).'));
        o.datatype = 'float'; o.default = '1.01';

        o = moptDyn('advanced', form.Value, 'bufferbloat_detection_window',
            _('Bufferbloat Detection Window'),
            _('Number of consecutive ping samples examined for delay.'));
        o.datatype = 'uinteger'; o.default = '6';

        o = moptDyn('advanced', form.Value, 'bufferbloat_detection_thr',
            _('Bufferbloat Detection Threshold'),
            _('How many samples in the window must show delay before bufferbloat is declared.'));
        o.datatype = 'uinteger'; o.default = '3';

        o = moptDyn('advanced', form.Value, 'high_load_thr',
            _('High Load Threshold (fraction)'),
            _('Fraction of current shaper rate above which load is "high" (e.g. 0.75).'));
        o.datatype = 'float'; o.default = '0.75';

        o = moptDyn('advanced', form.Value, 'bufferbloat_refractory_period_ms',
            _('Bufferbloat Refractory Period (ms)'),
            _('Minimum time between consecutive rate-down adjustments.'));
        o.datatype = 'uinteger'; o.default = '300';

        o = moptDyn('advanced', form.Value, 'decay_refractory_period_ms',
            _('Decay Refractory Period (ms)'),
            _('Minimum time between idle/low-load rate adjustments.'));
        o.datatype = 'uinteger'; o.default = '1000';

        o = moptDyn('advanced', form.Flag, 'enable_sleep_function',
            _('Enable Sleep on Sustained Idle'),
            _('Pause the active pingers after the link has been idle for the ' +
              'sustained-idle threshold. Saves CPU and ICMP traffic; pinging ' +
              'resumes automatically when traffic returns.'));
        o.default = '1';

        o = moptDyn('advanced', form.Value, 'sustained_idle_sleep_thr_s',
            _('Idle Sleep Threshold (s)'),
            _('How long both directions must stay below the connection-active ' +
              'threshold before the sleep function pauses the pingers.'));
        o.datatype = 'float'; o.default = '60.0';

        o = moptDyn('advanced', form.Flag, 'min_shaper_rates_enforcement',
            _('Enforce Min Rates on Idle / Stall'),
            _('When enabled, the shaper will not drop below the configured minimum rates.'));
        o.default = '0';

        o = moptDyn('advanced', form.Flag, 'qdisc_stats_debug',
            _('Debug CAKE Statistics Reads'),
            _('Log a line whenever live CAKE per-tin statistics cannot be ' +
              'read from the kernel (errno and tin count). Enable for a ' +
              'debugging session, then disable. Off by default.'));
        o.default = '0';

        o = moptDyn('advanced', form.Value, 'stall_detection_thr',
            _('Stall Detection Threshold (missed pings)'),
            _('Number of missed ping rounds (per-reflector intervals) without any ' +
              'reflector response before the connection is declared stalled. The ' +
              'stall state is shown in the live status block.'));
        o.datatype = 'uinteger'; o.default = '5';

        o = moptDyn('advanced', form.Value, 'connection_stall_thr_kbps',
            _('Connection Stall Rate Threshold (kbps)'),
            _('If both DL and UL are below this while pings time out, declare a stall.'));
        o.datatype = 'uinteger'; o.default = '10';

        o = moptDyn('advanced', form.Value, 'global_ping_response_timeout_s',
            _('Global Ping Response Timeout (s)'),
            _('Ultimate no-response guard: if no reflector has replied for this ' +
              'long, the connection is treated as down and the shaper rates are ' +
              'dropped to their minimums. Default: 10.'));
        o.datatype = 'float'; o.default = '10.0';

        o = moptDyn('advanced', form.Value, 'startup_wait_s',
            _('Startup Wait (s)'),
            _('Seconds to pause after start before adjusting rates.'));
        o.datatype = 'float'; o.default = '0.0';

        o = moptDyn('advanced', form.Value, 'monitor_achieved_rates_interval_ms',
            _('Rate Monitor Interval (ms)'),
            _('How often achieved DL/UL rates are sampled from sysfs.'));
        o.datatype = 'uinteger'; o.default = '200';

        o = moptDyn('advanced', form.Value, 'if_up_check_interval_s',
            _('Interface Up-Check Interval (s)'),
            _('How often the WAN interface is checked for presence. If it ' +
              'disappears (e.g. a PPPoE/DHCP reconnect), shaping and pinging are ' +
              'paused and recreated automatically when it returns.'));
        o.datatype = 'float'; o.default = '10.0';

        /* ════════════════════════════════════════════════════
         * Reflector Health tab (modal)
         * ════════════════════════════════════════════════════ */
        o = moptDyn('health', form.Value, 'reflector_health_check_interval_s',
            _('Health Check Interval (s)'),
            _('How often each reflector is checked for missed responses.'));
        o.datatype = 'float'; o.default = '1';

        o = moptDyn('health', form.Value, 'reflector_response_deadline_s',
            _('Response Deadline (s)'),
            _('A reflector is counted as non-responsive if it has not replied within this time.'));
        o.datatype = 'float'; o.default = '1';

        o = moptDyn('health', form.Value, 'reflector_misbehaving_detection_window',
            _('Misbehaving Detection Window (checks)'),
            _('Sliding window size for counting missed-response events per reflector.'));
        o.datatype = 'uinteger'; o.default = '60';

        o = moptDyn('health', form.Value, 'reflector_misbehaving_detection_thr',
            _('Misbehaving Detection Threshold'),
            _('Missed responses within the window required to trigger automatic replacement.'));
        o.datatype = 'uinteger'; o.default = '3';

        o = moptDyn('health', form.Value, 'reflector_replacement_interval_s',
            _('Reflector Replacement Interval (s)'),
            _('Minimum time between automatic replacements. Default: 3600 (1 hour).'));
        o.datatype = 'uinteger'; o.default = '3600';

        o = moptDyn('health', form.Value, 'reflector_comparison_interval_s',
            _('Reflector Comparison Interval (s)'),
            _('How often active reflectors are compared against spare candidates.'));
        o.datatype = 'uinteger'; o.default = '60';

        o = moptDyn('health', form.Value, 'reflector_sum_owd_baselines_delta_thr_ms',
            _('OWD Baselines Sum Delta Threshold (ms)'),
            _('Replace a reflector if its OWD baseline sum exceeds a spare\'s by more than this.'));
        o.datatype = 'float'; o.default = '20.0';

        o = moptDyn('health', form.Value, 'reflector_owd_delta_ewma_delta_thr_ms',
            _('OWD EWMA Delta Threshold (ms)'),
            _('Replacement threshold based on the OWD-delta EWMA.'));
        o.datatype = 'float'; o.default = '10.0';

        /*
         * Inject the Start/Stop/Restart buttons and the live status
         * (same per-instance blocks as the Overview widget) above the
         * instances grid.
         */
        return m.render().then(function(formNode) {
            var bar = E('div', { 'class': 'cbi-section' }, [
                E('h3', {}, _('Service Control')),
                E('div', { 'class': 'cbi-value' }, [
                    E('div', { 'class': 'cbi-value-title' }, ''),
                    E('div', {
                        'class': 'cbi-value-field',
                        'style': 'display:flex; gap:0.5em'
                    }, [
                        makeServiceButton(_('Start'),   'apply',  'start'),
                        makeServiceButton(_('Stop'),    'reset',  'stop'),
                        makeServiceButton(_('Restart'), 'reload', 'restart')
                    ])
                ])
            ]);

            var statusInner = E('div', {});
            var status = E('div', { 'class': 'cbi-section' }, [
                E('h3', {}, _('Live Status')),
                antilagStatus.render(statusInner, { tinToggle: true })
            ]);

            return E('div', {}, [ bar, status, formNode ]);
        });
    }
});
