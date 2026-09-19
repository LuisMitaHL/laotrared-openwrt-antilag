/*
 * cake-autorate – C rewrite for OpenWrt
 *
 * Standalone CAKE autorate daemon: no SQM scripts required.
 *
 * Two instance modes (per UCI section, `option mode`):
 *   dynamic – adaptive OWD-driven shaping, run as a procd daemon
 *   static  – fixed target rate, no reflectors/live changes, applied
 *             one-shot at service start and by the iface hotplug hook
 *
 * Lifecycle managed entirely in-process via tc_netlink.c:
 *   Startup  → tc_dl_setup() + tc_ul_setup()   (creates IFB, qdiscs, filters)
 *   Runtime  → tc_cake_set_bandwidth()          (adjusts rates in-place)
 *   Shutdown → tc_dl_teardown() + tc_ul_teardown() (removes all TC objects)
 *
 * Subcommands:
 *   antilag run   <section>  adaptive daemon (dynamic instances)
 *   antilag apply <section>  one-shot install of a static instance
 *   antilag stop  <section>  tear down an instance's qdiscs
 *
 * Algorithm mirrors cake-autorate.sh:
 *   • ICMP ping via raw IPv4 socket (in-process, no external pinger binary)
 *     – ping_type 0: ICMP Echo (type 8/0) – RTT/2, symmetric assumption
 *     – ping_type 1: ICMP Timestamp (type 13/14) – true per-direction OWD
 *   • Rate monitor via /sys/class/net polling
 *   • OWD EWMA baseline + delta sliding window for bufferbloat detection
 *   • Reflector health monitoring with automatic replacement
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <syslog.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>

#include <libubox/uloop.h>

#include "config.h"
#include "rate_monitor.h"
#include "tc_netlink.h"

/* ────────────────────────────────────────────────────────────── */
/*  Constants                                                     */
/* ────────────────────────────────────────────────────────────── */
#define DIR_DL 0
#define DIR_UL 1

#define LOAD_IDLE 0
#define LOAD_LOW  1
#define LOAD_HIGH 2
#define LOAD_BB   3  /* bufferbloat */

#define STATE_RUNNING 0
#define STATE_IDLE    1
#define STATE_STALL   2

/* Hard cap on the misbehaving-detection window (offences[] array size). */
#define MAX_OFFENCE_WINDOW 64

/* ────────────────────────────────────────────────────────────── */
/*  ICMP type 13/14 (Timestamp) definitions                       */
/* ────────────────────────────────────────────────────────────── */

/* Guard: some older OpenWrt SDK header snapshots omit these. */
#ifndef ICMP_TIMESTAMP
#define ICMP_TIMESTAMP      13
#define ICMP_TIMESTAMPREPLY 14
#endif

/*
 * ICMP Timestamp wire format (RFC 792).
 * All timestamp fields are milliseconds since midnight UT, big-endian.
 * Follows the standard icmphdr (8 bytes).
 */
struct icmp_ts_body {
    uint32_t originate;  /* sender's transmit time (echoed back by reflector) */
    uint32_t receive;    /* reflector receive time  (0 in request)             */
    uint32_t transmit;   /* reflector transmit time (0 in request)             */
};

/* Milliseconds per day – timestamp wraps at this value */
#define MS_PER_DAY 86400000UL

/*
 * PING_SEQ_RING – sequence number ring for correlating type-13 replies.
 *
 * For ICMP echo we embed a monotonic timestamp directly in the payload.
 * For ICMP timestamp the 32-bit originate field holds ms-since-midnight,
 * which is too coarse and doesn't encode reflector index.  Instead we
 * store per-sequence metadata here, keyed by (seq % PING_SEQ_RING).
 *
 * With pings every ~50 ms and max RTT < 2 s we have ≤40 in-flight pings;
 * 256 slots provides a comfortable safety margin before wrap collision.
 */
#define PING_SEQ_RING 256

typedef struct {
    int64_t  t_sent_us;      /* monotonic µs at send time (for RTT if needed) */
    uint32_t originate_ms;   /* ms-since-midnight sent in originate field      */
    int      reflector_idx;  /* which active reflector this ping was sent to   */
} ping_seq_slot_t;

/* ────────────────────────────────────────────────────────────── */
/*  Per-reflector runtime state                                   */
/* ────────────────────────────────────────────────────────────── */
typedef struct {
    char    addr[64];
    uint32_t addr_be;
    int64_t dl_owd_baseline_us;
    int64_t ul_owd_baseline_us;
    int64_t dl_owd_delta_ewma_us;
    int64_t ul_owd_delta_ewma_us;
    int64_t last_response_us;        /* 0 = no response yet */
    int     offences[MAX_OFFENCE_WINDOW];
    int     offences_idx;
    int     sum_offences;
} reflector_t;

/* ────────────────────────────────────────────────────────────── */
/*  Global application state                                      */
/* ────────────────────────────────────────────────────────────── */
typedef struct {
    cake_config_t   cfg;
    rate_monitor_t  rm;
    tc_nl_ctx_t    *tc_nl;

    /* Shaper rates (kbps) */
    uint32_t shaper_rate_kbps[2];
    uint32_t last_shaper_rate_kbps[2];

    /* Achieved rates */
    uint32_t achieved_rate_kbps[2];
    int      achieved_rate_updated[2];

    /* Load classification */
    int      load_condition[2];
    int      bufferbloat_detected[2];
    int64_t  t_last_bufferbloat_us[2];
    int64_t  t_last_decay_us[2];

    /* OWD sliding window (allocated to cfg.bufferbloat_detection_window) */
    int     *dl_delays;
    int     *ul_delays;
    int64_t *dl_owd_deltas_us;
    int64_t *ul_owd_deltas_us;
    int      delays_idx;
    int      delays_fill;
    int64_t  sum_dl_delays;
    int64_t  sum_ul_delays;
    int64_t  sum_dl_owd_deltas_us;
    int64_t  sum_ul_owd_deltas_us;
    int64_t  avg_owd_delta_us[2];

    /* Reflectors */
    reflector_t reflectors[MAX_REFLECTORS];
    int         no_active_reflectors;  /* = min(cfg.no_pingers, cfg.no_reflectors) */
    int         spare_idx;             /* next unused spare in cfg.reflectors[] */
    int64_t     t_last_reflector_health_us;
    int64_t     global_last_response_us;

    /* Integrated ICMP pinger (IPv4 raw socket) */
    int               icmp_sock;
    struct uloop_fd   icmp_ufd;
    struct uloop_timeout ping_timer;
    uint16_t          ping_id;
    uint16_t          ping_seq;
    int               ping_rr_idx;  /* round-robin reflector index */

    /* Per-sequence state for ICMP timestamp mode (type 13) */
    ping_seq_slot_t   ping_seq_ring[PING_SEQ_RING];

    /* Timers */
    struct uloop_timeout rate_timer;
    struct uloop_timeout health_timer;

    /* State machine */
    int main_state;

    /* Ping response interval (µs): ping_interval / no_pingers */
    int64_t ping_response_interval_us;

    /* Track whether setup succeeded (for teardown) */
    int dl_setup_done;
    int ul_setup_done;

    /* Feature: interface up/down recovery (if_up_check_interval_us) */
    struct uloop_timeout if_up_timer;
    int                  link_up;        /* 1 = WAN interface is currently up */

    /* Status file – written every rate-monitor tick for LuCI display.
     * Per instance so multiple daemons (multi-WAN) don't collide. */
    char status_path[96];       /* /var/run/antilag-<instance>.json     */
    char status_tmp_path[104];  /* /var/run/antilag-<instance>.json.tmp */
    int64_t t_started_us;       /* monotonic start time for uptime   */
} autorate_t;

/* ────────────────────────────────────────────────────────────── */
/*  Helpers                                                       */
/* ────────────────────────────────────────────────────────────── */
static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
 * instance_id_valid – UCI section names should already be limited to
 * [A-Za-z0-9_], but the daemon receives the section name from argv and
 * embeds it in a filesystem path.  Enforce the safe charset here so a
 * crafted value can never escape /var/run.
 */
static int instance_id_valid(const char *id)
{
    if (!id || !*id)
        return 0;
    for (const char *p = id; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

/* ────────────────────────────────────────────────────────────── */
/*  Static-instance state file                                    */
/*                                                                */
/*  Static instances have no daemon to remember which interfaces  */
/*  they shaped, so apply writes them to                       */
/*  /var/run/antilag-<section>.state.  stop reads that file so it */
/*  can tear the qdiscs down even after the UCI section is gone.  */
/*  Mirrors sqm-scripts' /var/run/SQM state files.                */
/* ────────────────────────────────────────────────────────────── */

static void state_path_for(const char *section, char *out, size_t n)
{
    snprintf(out, n, "/var/run/antilag-%s.state", section);
}

static void write_state_file(const cake_config_t *c)
{
    char path[96];
    state_path_for(c->instance_id, path, sizeof(path));

    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "dl_if=%s\nul_if=%s\n", c->dl_if, c->ul_if);
    fclose(f);
}

/*
 * read_state_file – load dl_if/ul_if from the per-section state file.
 * Returns 0 when at least one field was read, -1 otherwise.
 */
static int read_state_file(const char *section,
                           char *dl_if, size_t dl_n,
                           char *ul_if, size_t ul_n)
{
    char path[96];
    state_path_for(section, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    char line[128];
    int  got = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "dl_if=", 6) == 0) {
            snprintf(dl_if, dl_n, "%s", line + 6);
            /* strip trailing whitespace/newline */
            for (char *p = dl_if; *p; p++)
                if (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') { *p = '\0'; break; }
            got++;
        } else if (strncmp(line, "ul_if=", 6) == 0) {
            snprintf(ul_if, ul_n, "%s", line + 6);
            for (char *p = ul_if; *p; p++)
                if (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') { *p = '\0'; break; }
            got++;
        }
    }
    fclose(f);
    return got ? 0 : -1;
}

/* ────────────────────────────────────────────────────────────── */
/*  Runtime status file  (/var/run/antilag-<instance>.json)          */
/*                                                                */
/*  Written on every rate-monitor tick (~200 ms).                 */
/*  Read by the LuCI Overview widget via file.read RPC.           */
/*  Uses atomic rename(tmp→final) to avoid partial reads.         */
/* ────────────────────────────────────────────────────────────── */

static const char *load_str(int cond)
{
    switch (cond) {
        case LOAD_LOW:  return "low";
        case LOAD_HIGH: return "high";
        case LOAD_BB:   return "bufferbloat";
        default:        return "idle";
    }
}

static const char *state_str(int s)
{
    switch (s) {
        case STATE_IDLE:  return "idle";
        case STATE_STALL: return "stall";
        default:          return "running";
    }
}

/*
 * fprint_qdisc_stats – emit live per-tin CAKE statistics as a JSON object.
 * Called for each direction (dl/ul) that has a running CAKE qdisc.
 * Preceded by a comma so it slots into the status document directly.
 */
static void fprint_qdisc_stats(FILE *f, const char *key,
                               const cake_qdisc_stats_t *st)
{
    fprintf(f,
        ",\n"
        "  \"%s\": {\n"
        "    \"kind\": \"%s\",\n"
        "    \"tin_cnt\": %d,\n"
        "    \"capacity_bps\": %llu,\n"
        "    \"memory_limit\": %u,\n"
        "    \"memory_used\": %u,\n"
        "    \"active_queues\": %u,\n"
        "    \"tins\": [\n",
        key, st->kind, st->tin_cnt,
        (unsigned long long)st->capacity_bps,
        st->memory_limit, st->memory_used, st->active_queues);

    for (int i = 0; i < st->tin_cnt; i++) {
        const cake_tin_stats_t *t = &st->tins[i];
        fprintf(f,
            "      { \"threshold_bps\": %llu, \"sent_packets\": %u, \"sent_bytes\": %llu, "
            "\"dropped_packets\": %u, \"dropped_bytes\": %llu, "
            "\"ecn_packets\": %u, \"ecn_bytes\": %llu, "
            "\"backlog_bytes\": %u, \"target_us\": %u, \"interval_us\": %u, "
            "\"peak_delay_us\": %u, \"avg_delay_us\": %u, \"base_delay_us\": %u, "
            "\"way_misses\": %u, \"way_collisions\": %u, "
            "\"sparse_flows\": %u, \"bulk_flows\": %u, \"unresp_flows\": %u }%s\n",
            (unsigned long long)t->threshold_bps,
            t->sent_packets, (unsigned long long)t->sent_bytes,
            t->dropped_packets, (unsigned long long)t->dropped_bytes,
            t->ecn_packets, (unsigned long long)t->ecn_bytes,
            t->backlog_bytes, t->target_us, t->interval_us,
            t->peak_delay_us, t->avg_delay_us, t->base_delay_us,
            t->way_misses, t->way_collisions,
            t->sparse_flows, t->bulk_flows, t->unresp_flows,
            (i + 1 < st->tin_cnt) ? "," : "");
    }

    fprintf(f, "    ]\n  }");
}

static void write_status_file(autorate_t *ar)
{
    /*
     * Query live qdisc statistics from the kernel for each direction.
     * Failures (qdisc not up yet, interface gone, foreign qdisc) simply
     * omit the respective section; the widget degrades gracefully.
     */
    cake_qdisc_stats_t dl_stats = { 0 }, ul_stats = { 0 };
    int have_dl = 0, have_ul = 0;

    /*
     * Gated debug: when qdisc_stats_debug is set, log each failed
     * stats read once per failure streak (errno + tin_cnt) so a
     * future debugging session can see why per-tin tables are empty.
     * Silent by default.
     */
    static int dbg_dl_logged = 0, dbg_ul_logged = 0;

    if (ar->tc_nl && ar->dl_setup_done) {
        int dl_err = 0;
        have_dl = (tc_cake_get_stats(ar->tc_nl, ar->cfg.dl_if, &dl_stats) == 0);
        if (!have_dl)
            dl_err = errno;
        if (ar->cfg.qdisc_stats_debug && !have_dl && !dbg_dl_logged) {
            dbg_dl_logged = 1;
            errno = dl_err;
            syslog(LOG_WARNING,
                   "qdisc_stats: DL %s read failed: %m (tin_cnt=%d)",
                   ar->cfg.dl_if, dl_stats.tin_cnt);
        } else if (have_dl) {
            dbg_dl_logged = 0;
        }
    }
    if (ar->tc_nl && ar->ul_setup_done) {
        int ul_err = 0;
        have_ul = (tc_cake_get_stats(ar->tc_nl, ar->cfg.ul_if, &ul_stats) == 0);
        if (!have_ul)
            ul_err = errno;
        if (ar->cfg.qdisc_stats_debug && !have_ul && !dbg_ul_logged) {
            dbg_ul_logged = 1;
            errno = ul_err;
            syslog(LOG_WARNING,
                   "qdisc_stats: UL %s read failed: %m (tin_cnt=%d)",
                   ar->cfg.ul_if, ul_stats.tin_cnt);
        } else if (have_ul) {
            dbg_ul_logged = 0;
        }
    }

    FILE *f = fopen(ar->status_tmp_path, "w");
    if (!f)
        return;

    int64_t uptime_s   = (now_us() - ar->t_started_us) / 1000000LL;
    long dl_owd_ms10   = (long)((ar->avg_owd_delta_us[DIR_DL] + 50LL) / 100LL);
    long ul_owd_ms10   = (long)((ar->avg_owd_delta_us[DIR_UL] + 50LL) / 100LL);

    fprintf(f,
        "{\n"
        "  \"instance\": \"%s\",\n"
        "  \"mode\": \"dynamic\",\n"
        "  \"state\": \"%s\",\n"
        "  \"link_up\": %d,\n"
        "  \"dl_if\": \"%s\",\n"
        "  \"ul_if\": \"%s\",\n"
        "  \"shaper_dl_kbps\": %u,\n"
        "  \"shaper_ul_kbps\": %u,\n"
        "  \"achieved_dl_kbps\": %u,\n"
        "  \"achieved_ul_kbps\": %u,\n"
        "  \"load_dl\": \"%s\",\n"
        "  \"load_ul\": \"%s\",\n"
        "  \"bb_dl\": %d,\n"
        "  \"bb_ul\": %d,\n"
        "  \"avg_owd_dl_ms10\": %ld,\n"
        "  \"avg_owd_ul_ms10\": %ld,\n"
        "  \"active_reflectors\": %d,\n"
        "  \"uptime_s\": %lld",
        ar->cfg.instance_id,
        state_str(ar->main_state),
        ar->link_up,
        ar->cfg.dl_if,
        ar->cfg.ul_if,
        ar->shaper_rate_kbps[DIR_DL],
        ar->shaper_rate_kbps[DIR_UL],
        ar->achieved_rate_kbps[DIR_DL],
        ar->achieved_rate_kbps[DIR_UL],
        load_str(ar->load_condition[DIR_DL]),
        load_str(ar->load_condition[DIR_UL]),
        ar->bufferbloat_detected[DIR_DL],
        ar->bufferbloat_detected[DIR_UL],
        dl_owd_ms10,
        ul_owd_ms10,
        ar->no_active_reflectors,
        (long long)uptime_s
    );

    if (have_dl)
        fprint_qdisc_stats(f, "cake_dl", &dl_stats);
    if (have_ul)
        fprint_qdisc_stats(f, "cake_ul", &ul_stats);

    fprintf(f, "\n}\n");

    fclose(f);
    rename(ar->status_tmp_path, ar->status_path);
}

/*
 * write_static_status – minimal status document for a stateless static
 * instance.  There is no daemon to refresh it, so it contains only the
 * fixed shaper rates and which directions were installed; the LuCI
 * widget renders it as a compact "Static" block without live metrics.
 */
static void write_static_status(const cake_config_t *c, int dl_up, int ul_up)
{
    char path[96], tmp[104];
    snprintf(path, sizeof(path), "/var/run/antilag-%s.json", c->instance_id);
    snprintf(tmp,  sizeof(tmp),  "/var/run/antilag-%s.json.tmp", c->instance_id);

    FILE *f = fopen(tmp, "w");
    if (!f)
        return;

    fprintf(f,
        "{\n"
        "  \"instance\": \"%s\",\n"
        "  \"mode\": \"static\",\n"
        "  \"state\": \"static\",\n"
        "  \"link_up\": %d,\n"
        "  \"dl_if\": \"%s\",\n"
        "  \"ul_if\": \"%s\",\n"
        "  \"shaper_dl_kbps\": %u,\n"
        "  \"shaper_ul_kbps\": %u,\n"
        "  \"achieved_dl_kbps\": 0,\n"
        "  \"achieved_ul_kbps\": 0,\n"
        "  \"load_dl\": \"idle\",\n"
        "  \"load_ul\": \"idle\",\n"
        "  \"bb_dl\": 0,\n"
        "  \"bb_ul\": 0,\n"
        "  \"avg_owd_dl_ms10\": 0,\n"
        "  \"avg_owd_ul_ms10\": 0,\n"
        "  \"active_reflectors\": 0,\n"
        "  \"uptime_s\": 0,\n"
        "  \"dl_active\": %d,\n"
        "  \"ul_active\": %d\n"
        "}\n",
        c->instance_id,
        (dl_up || ul_up) ? 1 : 0,
        c->dl_if, c->ul_if,
        dl_up ? c->base_dl_shaper_rate_kbps : 0,
        ul_up ? c->base_ul_shaper_rate_kbps : 0,
        dl_up, ul_up);

    fclose(f);
    rename(tmp, path);
}

/* ────────────────────────────────────────────────────────────── */
/*  Build cake_qdisc_opts_t from config                           */
/* ────────────────────────────────────────────────────────────── */

/*
 * make_dl_opts – options for the IFB (download) CAKE qdisc.
 *
 *  .ingress = 1  tells CAKE it is on an ingress (IFB) path.
 *  .wash    = 0  we preserve DSCP on DL so the local stack still
 *               sees original markings; washing only makes sense UL.
 */
static cake_qdisc_opts_t make_dl_opts(const cake_config_t *c)
{
    cake_qdisc_opts_t o;
    memset(&o, 0, sizeof(o));
    o.overhead   = c->cake_overhead;
    o.mpu        = c->cake_mpu;
    o.nat        = c->cake_nat;
    o.wash       = 0;                 /* never wash on DL/IFB */
    o.ingress    = 1;                 /* always set for IFB   */
    o.ack_filter = 0;                 /* ACK filtering is UL-only */
    o.diffserv   = (uint32_t)c->cake_diffserv;
    o.flow_mode  = (uint32_t)c->cake_dl_flow_mode;
    o.atm        = (uint32_t)c->cake_atm;
    o.rtt_us     = c->cake_rtt_us;
    o.split_gso  = (uint32_t)c->cake_split_gso;
    return o;
}

/*
 * make_ul_opts – options for the WAN (upload) CAKE qdisc.
 *
 *  .ingress = 0  normal egress qdisc.
 *  .wash    = per-config (strip DSCP on UL by default).
 */
static cake_qdisc_opts_t make_ul_opts(const cake_config_t *c)
{
    cake_qdisc_opts_t o;
    memset(&o, 0, sizeof(o));
    o.overhead   = c->cake_overhead;
    o.mpu        = c->cake_mpu;
    o.nat        = c->cake_nat;
    o.wash       = (uint32_t)c->cake_wash;
    o.ingress    = 0;
    o.ack_filter = (uint32_t)c->cake_ack_filter;
    o.diffserv   = (uint32_t)c->cake_diffserv;
    o.flow_mode  = (uint32_t)c->cake_ul_flow_mode;
    o.atm        = (uint32_t)c->cake_atm;
    o.rtt_us     = c->cake_rtt_us;
    o.split_gso  = (uint32_t)c->cake_split_gso;
    return o;
}

/* ────────────────────────────────────────────────────────────── */
/*  CAKE setup / teardown                                         */
/* ────────────────────────────────────────────────────────────── */

/*
 * cake_setup – create the full DL + UL CAKE plumbing.
 *
 * DL: IFB created, brought up, CAKE attached, ingress qdisc on WAN,
 *     match-all mirred redirect filter WAN→IFB.
 * UL: CAKE root qdisc attached to WAN.
 *
 * Sets ar->dl_setup_done / ul_setup_done so teardown knows what to undo.
 */
static int cake_setup(autorate_t *ar)
{
    cake_config_t *c = &ar->cfg;

    if (c->adjust_dl_shaper_rate && c->dl_if[0]) {
        cake_qdisc_opts_t dl_opts = make_dl_opts(c);
        if (tc_dl_setup(ar->tc_nl,
                        c->ul_if,          /* WAN interface */
                        c->dl_if,          /* IFB interface */
                        ar->shaper_rate_kbps[DIR_DL],
                        &dl_opts) < 0) {
            syslog(LOG_ERR, "cake_setup: DL path failed: %m");
            return -1;
        }
        ar->dl_setup_done = 1;
        ar->last_shaper_rate_kbps[DIR_DL] = ar->shaper_rate_kbps[DIR_DL];
    }

    if (c->adjust_ul_shaper_rate && c->ul_if[0]) {
        cake_qdisc_opts_t ul_opts = make_ul_opts(c);
        if (tc_ul_setup(ar->tc_nl,
                        c->ul_if,
                        ar->shaper_rate_kbps[DIR_UL],
                        &ul_opts) < 0) {
            syslog(LOG_WARNING,
                   "cake_setup: UL path deferred (will retry): %m");
        } else {
            ar->ul_setup_done = 1;
            ar->last_shaper_rate_kbps[DIR_UL] = ar->shaper_rate_kbps[DIR_UL];
        }
    }

    return 0;
}

/*
 * cake_teardown – remove all CAKE TC objects created by cake_setup().
 *
 * Called on graceful shutdown.  Leaves interfaces in a clean state
 * without any rate limiting.
 */
static void cake_teardown(autorate_t *ar)
{
    cake_config_t *c = &ar->cfg;

    if (ar->dl_setup_done) {
        tc_dl_teardown(ar->tc_nl, c->ul_if, c->dl_if);
        ar->dl_setup_done = 0;
    }

    if (ar->ul_setup_done) {
        tc_ul_teardown(ar->tc_nl, c->ul_if);
        ar->ul_setup_done = 0;
    }
}

/* ────────────────────────────────────────────────────────────── */
/*  Static instances – one-shot apply / stop                      */
/*                                                                */
/*  Static instances are the sqm-scripts model: a fixed target    */
/*  download/upload rate, no reflectors, no live adjustment and   */
/*  no long-running daemon.  The init script applies them at      */
/*  service start and the interface hotplug hook re-applies them  */
/*  whenever the WAN interface comes back up.                     */
/* ────────────────────────────────────────────────────────────── */

/*
 * instance_stop – remove the CAKE plumbing for a section.
 *
 * Interface names come from UCI when the section still exists, and
 * otherwise from the state file written by static_apply().  This lets
 * `stop` clean up after a section has been deleted from the config.
 * Safe to call with no qdiscs present.
 */
static int instance_stop(const char *section)
{
    cake_config_t c;
    char dl_if[MAX_IF_NAME] = "";
    char ul_if[MAX_IF_NAME] = "";
    char path[96];
    int  have = 0;

    if (!instance_id_valid(section)) {
        syslog(LOG_ERR, "stop: invalid section name '%s'", section);
        return 1;
    }

    if (config_load(section, &c) == 0) {
        snprintf(dl_if, sizeof(dl_if), "%s", c.dl_if);
        snprintf(ul_if, sizeof(ul_if), "%s", c.ul_if);
        have = 1;
    }
    if (read_state_file(section, dl_if, sizeof(dl_if),
                        ul_if, sizeof(ul_if)) == 0)
        have = 1;

    if (have && ul_if[0]) {
        tc_nl_ctx_t *nl = tc_nl_open();
        if (!nl) {
            syslog(LOG_ERR, "stop: netlink open failed: %m");
        } else {
            if (dl_if[0])
                tc_dl_teardown(nl, ul_if, dl_if);
            tc_ul_teardown(nl, ul_if);
            tc_nl_close(nl);
        }
    }

    snprintf(path, sizeof(path), "/var/run/antilag-%s.json", section);
    unlink(path);
    state_path_for(section, path, sizeof(path));
    unlink(path);

    return 0;
}

/*
 * static_apply – install the fixed-rate CAKE qdiscs for one static
 * instance and exit.  Idempotent: the underlying tc_dl_setup /
 * tc_ul_setup calls tolerate existing objects, so a hotplug re-apply
 * is safe.
 */
static int static_apply(const char *section)
{
    cake_config_t c;
    tc_nl_ctx_t  *nl;
    int           dl_done = 0;
    int           ul_done = 0;

    if (!instance_id_valid(section)) {
        syslog(LOG_ERR, "apply: invalid section name '%s'", section);
        return 1;
    }

    if (config_load(section, &c) < 0) {
        syslog(LOG_ERR, "apply: cannot load config section '%s'", section);
        return 1;
    }

    /* Dynamic instances are owned by the procd daemon. */
    if (c.mode != MODE_STATIC)
        return 0;

    if (!c.enabled)
        return instance_stop(section);

    if (!c.ul_if[0]) {
        syslog(LOG_ERR, "apply: static instance '%s' has no ul_if", section);
        return 1;
    }

    nl = tc_nl_open();
    if (!nl) {
        syslog(LOG_ERR, "apply: netlink open failed: %m");
        return 1;
    }

    if (c.dl_if[0]) {
        cake_qdisc_opts_t o = make_dl_opts(&c);
        if (tc_dl_setup(nl, c.ul_if, c.dl_if,
                        c.base_dl_shaper_rate_kbps, &o) < 0)
            syslog(LOG_ERR, "apply: DL setup failed on %s (ifb %s): %m",
                   c.ul_if, c.dl_if);
        else
            dl_done = 1;
    }

    {
        cake_qdisc_opts_t o = make_ul_opts(&c);
        if (tc_ul_setup(nl, c.ul_if, c.base_ul_shaper_rate_kbps, &o) < 0)
            syslog(LOG_ERR, "apply: UL setup failed on %s: %m", c.ul_if);
        else
            ul_done = 1;
    }

    tc_nl_close(nl);

    if (!dl_done && !ul_done) {
        syslog(LOG_ERR,
               "apply: no CAKE qdisc could be installed for '%s' "
               "(interface down?)", section);
        return 1;
    }

    write_state_file(&c);
    write_static_status(&c, dl_done, ul_done);

    syslog(LOG_INFO,
           "static shaping applied: '%s' dl=%s/%ukbps ul=%s/%ukbps",
           section,
           dl_done ? c.dl_if : "-",
           dl_done ? c.base_dl_shaper_rate_kbps : 0,
           ul_done ? c.ul_if : "-",
           ul_done ? c.base_ul_shaper_rate_kbps : 0);
    return 0;
}

/* ────────────────────────────────────────────────────────────── */
/*  CAKE rate control                                             */
/* ────────────────────────────────────────────────────────────── */
static void clamp_shaper_rate(autorate_t *ar, int dir)
{
    uint32_t mn = (dir == DIR_DL) ? ar->cfg.min_dl_shaper_rate_kbps
                                  : ar->cfg.min_ul_shaper_rate_kbps;
    uint32_t mx = (dir == DIR_DL) ? ar->cfg.max_dl_shaper_rate_kbps
                                  : ar->cfg.max_ul_shaper_rate_kbps;
    if (ar->shaper_rate_kbps[dir] < mn) ar->shaper_rate_kbps[dir] = mn;
    if (ar->shaper_rate_kbps[dir] > mx) ar->shaper_rate_kbps[dir] = mx;
}

static void set_shaper_rate(autorate_t *ar, int dir)
{
    uint32_t rate     = ar->shaper_rate_kbps[dir];
    uint32_t old_rate = ar->last_shaper_rate_kbps[dir];

    if (rate == old_rate)
        return;

    /* Hysteresis: skip trivial changes (< 0.5%) unless hitting a rail. */
    uint32_t min_limit = (dir == DIR_DL) ? ar->cfg.min_dl_shaper_rate_kbps
                                         : ar->cfg.min_ul_shaper_rate_kbps;
    uint32_t max_limit = (dir == DIR_DL) ? ar->cfg.max_dl_shaper_rate_kbps
                                         : ar->cfg.max_ul_shaper_rate_kbps;

    uint32_t diff = (rate > old_rate) ? (rate - old_rate) : (old_rate - rate);
    uint32_t base = old_rate ? old_rate : rate;
    uint32_t threshold = base / 200;
    uint32_t floor_val = (base < 5000) ? (base / 100) : 50;
    if (threshold < floor_val) threshold = floor_val;

    if (diff < threshold && rate != min_limit && rate != max_limit)
        return;

    const char *iface  = (dir == DIR_DL) ? ar->cfg.dl_if : ar->cfg.ul_if;
    int         adjust = (dir == DIR_DL) ? ar->cfg.adjust_dl_shaper_rate
                                         : ar->cfg.adjust_ul_shaper_rate;

    if (adjust && iface[0] != '\0') {
        if (tc_cake_set_bandwidth(ar->tc_nl, iface, rate) < 0)
            return;
    }

    ar->last_shaper_rate_kbps[dir] = rate;
}

/* ────────────────────────────────────────────────────────────── */
/*  Rate adjustment                                               */
/* ────────────────────────────────────────────────────────────── */
static void adjust_shaper_rate(autorate_t *ar, int dir, int64_t t_now_us)
{
    cake_config_t *c    = &ar->cfg;
    uint32_t base       = (dir == DIR_DL) ? c->base_dl_shaper_rate_kbps
                                           : c->base_ul_shaper_rate_kbps;
    int64_t delay_thr   = (dir == DIR_DL) ? c->dl_owd_delta_delay_thr_us
                                           : c->ul_owd_delta_delay_thr_us;
    int64_t max_up_thr  = (dir == DIR_DL) ? c->dl_avg_owd_delta_max_adjust_up_thr_us
                                           : c->ul_avg_owd_delta_max_adjust_up_thr_us;
    int64_t max_down_thr = (dir == DIR_DL) ? c->dl_avg_owd_delta_max_adjust_down_thr_us
                                            : c->ul_avg_owd_delta_max_adjust_down_thr_us;

    switch (ar->load_condition[dir]) {

    case LOAD_BB: {
        if (t_now_us - ar->t_last_bufferbloat_us[dir] <
                c->bufferbloat_refractory_period_us)
            break;

        int64_t avg = ar->avg_owd_delta_us[dir];
        int64_t factor;
        if (max_down_thr <= delay_thr) {
            factor = c->shaper_rate_max_adjust_down_bufferbloat;
        } else if (avg > delay_thr) {
            factor = c->shaper_rate_min_adjust_down_bufferbloat
                + (c->shaper_rate_max_adjust_down_bufferbloat
                   - c->shaper_rate_min_adjust_down_bufferbloat)
                * (avg - delay_thr)
                / (max_down_thr - delay_thr);
        } else {
            factor = c->shaper_rate_min_adjust_down_bufferbloat;
        }

        ar->shaper_rate_kbps[dir] =
            (uint32_t)((uint64_t)ar->shaper_rate_kbps[dir]
                       * (uint64_t)factor / 1000000ULL);
        ar->t_last_bufferbloat_us[dir] = t_now_us;
        ar->t_last_decay_us[dir]       = t_now_us;
        break;
    }

    case LOAD_HIGH: {
        if (!ar->achieved_rate_updated[dir])
            break;
        if (t_now_us - ar->t_last_bufferbloat_us[dir] <
                c->bufferbloat_refractory_period_us)
            break;

        int64_t avg = ar->avg_owd_delta_us[dir];
        int64_t factor;
        /*
         *   avg <= max_up_thr (10ms) → full rate increase (1.04×)
         *   max_up_thr < avg < delay_thr (30ms) → interpolate down
         *   avg >= delay_thr → no increase (1.00×)
         */
        if (avg <= max_up_thr) {
            factor = c->shaper_rate_max_adjust_up_load_high;
        } else if (avg < delay_thr && max_up_thr < delay_thr) {
            /* interpolate from max down to min as avg approaches delay_thr */
            factor = c->shaper_rate_max_adjust_up_load_high
                - (c->shaper_rate_max_adjust_up_load_high
                   - c->shaper_rate_min_adjust_up_load_high)
                * (avg - max_up_thr)
                / (delay_thr - max_up_thr);
        } else {
            factor = c->shaper_rate_min_adjust_up_load_high;
        }

        ar->shaper_rate_kbps[dir] =
            (uint32_t)((uint64_t)ar->shaper_rate_kbps[dir]
                       * (uint64_t)factor / 1000000ULL);
        ar->achieved_rate_updated[dir] = 0;
        ar->t_last_decay_us[dir]       = t_now_us;
        break;
    }

    case LOAD_LOW:
    case LOAD_IDLE: {
        if (t_now_us - ar->t_last_decay_us[dir] < c->decay_refractory_period_us)
            break;

        uint32_t rate = ar->shaper_rate_kbps[dir];
        if (rate > base) {
            int64_t f = c->shaper_rate_adjust_down_load_low;
            rate = (uint32_t)((uint64_t)rate * (uint64_t)f / 1000000ULL);
            ar->shaper_rate_kbps[dir] = (rate < base) ? base : rate;
        } else if (rate < base) {
            int64_t f = c->shaper_rate_adjust_up_load_low;
            rate = (uint32_t)((uint64_t)rate * (uint64_t)f / 1000000ULL);
            ar->shaper_rate_kbps[dir] = (rate > base) ? base : rate;
        }
        ar->t_last_decay_us[dir] = t_now_us;
        break;
    }
    }

    clamp_shaper_rate(ar, dir);
    set_shaper_rate(ar, dir);
}

/* ────────────────────────────────────────────────────────────── */
/*  OWD processing (called for each received ping response)       */
/* ────────────────────────────────────────────────────────────── */
static void process_owd(autorate_t *ar,
                        int reflector_idx,
                        int64_t dl_owd_us,
                        int64_t ul_owd_us,
                        int64_t t_now_us)
{
    cake_config_t *c = &ar->cfg;
    reflector_t   *r = &ar->reflectors[reflector_idx];

    if (r->dl_owd_baseline_us == 0) {
        r->dl_owd_baseline_us = dl_owd_us;
        r->ul_owd_baseline_us = ul_owd_us;
        r->last_response_us   = t_now_us;
        return;
    }

    if (dl_owd_us - r->dl_owd_baseline_us < -3000000LL ||
        ul_owd_us - r->ul_owd_baseline_us < -3000000LL) {
        r->dl_owd_baseline_us = dl_owd_us;
        r->ul_owd_baseline_us = ul_owd_us;
        r->last_response_us   = t_now_us;
        return;
    }

    int64_t dl_alpha = (dl_owd_us > r->dl_owd_baseline_us)
        ? c->alpha_baseline_increase
        : c->alpha_baseline_decrease;
    int64_t ul_alpha = (ul_owd_us > r->ul_owd_baseline_us)
        ? c->alpha_baseline_increase
        : c->alpha_baseline_decrease;

    r->dl_owd_baseline_us =
          dl_alpha * dl_owd_us           / 1000000LL
        + (1000000LL - dl_alpha) * r->dl_owd_baseline_us / 1000000LL;
    r->ul_owd_baseline_us =
          ul_alpha * ul_owd_us           / 1000000LL
        + (1000000LL - ul_alpha) * r->ul_owd_baseline_us / 1000000LL;

    int64_t dl_delta = dl_owd_us - r->dl_owd_baseline_us;
    int64_t ul_delta = ul_owd_us - r->ul_owd_baseline_us;

    if (ar->load_condition[DIR_DL] == LOAD_HIGH ||
        ar->load_condition[DIR_UL] == LOAD_HIGH) {
        int64_t ae = c->alpha_delta_ewma;
        r->dl_owd_delta_ewma_us =
              ae * dl_delta             / 1000000LL
            + (1000000LL - ae) * r->dl_owd_delta_ewma_us / 1000000LL;
        r->ul_owd_delta_ewma_us =
              ae * ul_delta             / 1000000LL
            + (1000000LL - ae) * r->ul_owd_delta_ewma_us / 1000000LL;
    }

    int bdw = c->bufferbloat_detection_window;
    int idx = ar->delays_idx;

    ar->sum_dl_delays -= ar->dl_delays[idx];
    ar->dl_delays[idx] = (dl_delta > c->dl_owd_delta_delay_thr_us) ? 1 : 0;
    ar->sum_dl_delays += ar->dl_delays[idx];

    ar->sum_ul_delays -= ar->ul_delays[idx];
    ar->ul_delays[idx] = (ul_delta > c->ul_owd_delta_delay_thr_us) ? 1 : 0;
    ar->sum_ul_delays += ar->ul_delays[idx];

    ar->sum_dl_owd_deltas_us -= ar->dl_owd_deltas_us[idx];
    ar->dl_owd_deltas_us[idx] = dl_delta;
    ar->sum_dl_owd_deltas_us += dl_delta;

    ar->sum_ul_owd_deltas_us -= ar->ul_owd_deltas_us[idx];
    ar->ul_owd_deltas_us[idx] = ul_delta;
    ar->sum_ul_owd_deltas_us += ul_delta;

    ar->delays_idx = (idx + 1) % bdw;

    /* Use the actual number of samples collected (capped at bdw)
     * to avoid dividing the sum by the full window during warmup, which
     * artificially deflates the average and suppresses rate increases. */
    if (ar->delays_fill < bdw)
        ar->delays_fill++;
    int divisor = ar->delays_fill;

    ar->avg_owd_delta_us[DIR_DL] = ar->sum_dl_owd_deltas_us / divisor;
    ar->avg_owd_delta_us[DIR_UL] = ar->sum_ul_owd_deltas_us / divisor;

    ar->bufferbloat_detected[DIR_DL] =
        (ar->sum_dl_delays >= c->bufferbloat_detection_thr);
    ar->bufferbloat_detected[DIR_UL] =
        (ar->sum_ul_delays >= c->bufferbloat_detection_thr);

    uint32_t high_thr_dl = (uint32_t)((uint64_t)ar->shaper_rate_kbps[DIR_DL]
                                      * (uint64_t)c->high_load_thr / 1000000ULL);
    uint32_t high_thr_ul = (uint32_t)((uint64_t)ar->shaper_rate_kbps[DIR_UL]
                                      * (uint64_t)c->high_load_thr / 1000000ULL);

    for (int d = 0; d < 2; d++) {
        uint32_t ach = ar->achieved_rate_kbps[d];
        uint32_t thr = (d == DIR_DL) ? high_thr_dl : high_thr_ul;
        if (ar->bufferbloat_detected[d])
            ar->load_condition[d] = LOAD_BB;
        else if (ach >= thr)
            ar->load_condition[d] = LOAD_HIGH;
        else if (ach >= c->connection_active_thr_kbps)
            ar->load_condition[d] = LOAD_LOW;
        else
            ar->load_condition[d] = LOAD_IDLE;
    }

    r->last_response_us            = t_now_us;
    ar->global_last_response_us    = t_now_us;

    adjust_shaper_rate(ar, DIR_DL, t_now_us);
    adjust_shaper_rate(ar, DIR_UL, t_now_us);
}

/* ────────────────────────────────────────────────────────────── */
/*  ICMP pinger – shared helpers                                  */
/* ────────────────────────────────────────────────────────────── */

#define PING_PAYLOAD_MAGIC 0xCACEB00Bu

/*
 * csum16 – RFC 1071 Internet checksum.
 */
static uint16_t csum16(const void *data, size_t len)
{
    const uint16_t *word = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += *word++;
        len -= 2;
    }
    if (len == 1) {
        uint16_t last = 0;
        *(uint8_t *)&last = *(const uint8_t *)word;
        sum += last;
    }
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    return (uint16_t)~sum;
}

static void write_be64(uint8_t out[8], uint64_t v)
{
    out[0]=(uint8_t)(v>>56); out[1]=(uint8_t)(v>>48);
    out[2]=(uint8_t)(v>>40); out[3]=(uint8_t)(v>>32);
    out[4]=(uint8_t)(v>>24); out[5]=(uint8_t)(v>>16);
    out[6]=(uint8_t)(v>> 8); out[7]=(uint8_t)(v);
}

static uint64_t read_be64(const uint8_t in[8])
{
    return ((uint64_t)in[0]<<56)|((uint64_t)in[1]<<48)|
           ((uint64_t)in[2]<<40)|((uint64_t)in[3]<<32)|
           ((uint64_t)in[4]<<24)|((uint64_t)in[5]<<16)|
           ((uint64_t)in[6]<< 8)|((uint64_t)in[7]);
}

/*
 * ms_since_midnight_realtime – milliseconds since 00:00:00 UTC today.
 * Used for the ICMP Timestamp originate field (RFC 792).
 * CLOCK_REALTIME is required because the reflector's timestamps are
 * also wall-clock based.
 */
static uint32_t ms_since_midnight_realtime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ms = (uint64_t)ts.tv_sec * 1000ULL
                + (uint64_t)ts.tv_nsec / 1000000ULL;
    return (uint32_t)(ms % MS_PER_DAY);
}

/*
 * ts_diff_ms – signed difference between two ms-since-midnight values.
 *
 * Handles midnight rollover: if the raw difference exceeds ±12 hours,
 * we assume the day boundary was crossed and correct by ±86400000.
 * For valid ping RTTs (< a few seconds) this is always correct.
 */
static int32_t ts_diff_ms(uint32_t later, uint32_t earlier)
{
    int32_t d = (int32_t)((int64_t)later - (int64_t)earlier);
    if (d >  43200000) d -= (int32_t)MS_PER_DAY;
    if (d < -43200000) d += (int32_t)MS_PER_DAY;
    return d;
}

/* ── ICMP Echo payload (type 8, ping_type 0) ──────────────────
 *
 * We embed a magic number and a 64-bit monotonic send timestamp so
 * that any reply that doesn't carry our payload is silently ignored.
 * No ring-buffer lookup needed – the timestamp is in the payload.
 */
struct ping_payload {
    uint32_t magic_be;
    uint16_t ridx_be;       /* reflector index (sanity check) */
    uint16_t reserved_be;
    uint8_t  t_sent_be64[8];
};

/* ────────────────────────────────────────────────────────────── */
/*  ICMP reply callback (handles both type 0 and type 14)         */
/* ────────────────────────────────────────────────────────────── */
static void icmp_reply_cb(struct uloop_fd *ufd, unsigned int events)
{
    (void)events;
    autorate_t *ar = container_of(ufd, autorate_t, icmp_ufd);

    for (;;) {
        uint8_t buf[1500];  /* full Ethernet MTU – prevents silent truncation */
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);

        ssize_t n = recvfrom(ufd->fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&src, &slen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            return;
        }
        if ((size_t)n < sizeof(struct iphdr)) continue;

        struct iphdr *iph = (struct iphdr *)buf;
        int ip_hlen = iph->ihl * 4;
        if (ip_hlen < 20 ||
            (size_t)n < (size_t)ip_hlen + sizeof(struct icmphdr))
            continue;

        struct icmphdr *icmph = (struct icmphdr *)(buf + ip_hlen);

        /* Filter by our ping ID */
        if (ntohs(icmph->un.echo.id) != ar->ping_id) continue;

        int64_t t_now_us = now_us();

        /* ── ICMP Echo Reply (type 0) – ping_type 0 ─────────── */
        if (icmph->type == ICMP_ECHOREPLY) {

            const uint8_t *payload = (const uint8_t *)(icmph + 1);
            size_t plen = (size_t)n - (size_t)ip_hlen - sizeof(*icmph);
            if (plen < sizeof(struct ping_payload)) continue;

            const struct ping_payload *pl = (const struct ping_payload *)payload;
            if (pl->magic_be != htonl(PING_PAYLOAD_MAGIC)) continue;

            /* Match reflector by source IP */
            uint32_t src_be = src.sin_addr.s_addr;
            int ridx = -1;
            for (int i = 0; i < ar->no_active_reflectors; i++) {
                if (ar->reflectors[i].addr_be == src_be) { ridx = i; break; }
            }
            if (ridx < 0) continue;

            int64_t t_sent_us = (int64_t)read_be64(pl->t_sent_be64);
            int64_t rtt_us    = t_now_us - t_sent_us;
            if (rtt_us <= 0) continue;

            /* RTT/2 – symmetric OWD estimate */
            int64_t owd_us = rtt_us / 2;
            process_owd(ar, ridx, owd_us, owd_us, t_now_us);
        }

        /* ── ICMP Timestamp Reply (type 14) – ping_type 1 ──────
         *
         * True per-direction OWD via RFC 792 ICMP Timestamp:
         *
         *   T1 = originate_ms  (our clock, ms-since-midnight)
         *   T2 = receive_ms    (reflector clock, ms-since-midnight)
         *   T3 = transmit_ms   (reflector clock, ms-since-midnight)
         *   T4 = local_rx_ms   (our clock, ms-since-midnight NOW)
         *
         *   UL raw  = T2 - T1   (absorbs clock offset θ as constant)
         *   DL raw  = T4 - T3   (absorbs -θ as constant)
         *
         * Because we use an asymmetric EWMA baseline that tracks the
         * running minimum, the constant clock offset cancels when
         * computing the delta from baseline.  No NTP synchronisation
         * with the reflector is required.
         *
         * If the reflector sets transmit = receive (zero processing
         * time), T3-T2 = 0 and the calculation degrades gracefully to
         * RTT/2 split by the measured asymmetry ratio.
         */
        else if (icmph->type == ICMP_TIMESTAMPREPLY) {

            size_t ts_body_off = (size_t)ip_hlen + sizeof(*icmph);
            if ((size_t)n < ts_body_off + sizeof(struct icmp_ts_body)) continue;

            const struct icmp_ts_body *tsb =
                (const struct icmp_ts_body *)(buf + ts_body_off);

            uint16_t seq = ntohs(icmph->un.echo.sequence);
            ping_seq_slot_t *slot = &ar->ping_seq_ring[seq % PING_SEQ_RING];

            /* Validate that this reply matches the slot we stored */
            if (slot->reflector_idx < 0 || slot->t_sent_us == 0) continue;

            /* Verify source IP matches what we expected for this slot */
            int ridx = slot->reflector_idx;
            if (ridx >= ar->no_active_reflectors ||
                ar->reflectors[ridx].addr_be != src.sin_addr.s_addr)
                continue;

            uint32_t orig_ms   = slot->originate_ms;
            uint32_t recv_ms   = ntohl(tsb->receive);
            uint32_t tx_ms     = ntohl(tsb->transmit);
            uint32_t local_ms  = ms_since_midnight_realtime();

            /*
             * Sanity: reject if receive < originate by more than 5 s
             * (would imply clocks are wildly out of sync or the reflector
             * is broken).  A 5 second tolerance covers any reasonable RTT.
             */
            int32_t ul_ms = ts_diff_ms(recv_ms, orig_ms);
            int32_t dl_ms = ts_diff_ms(local_ms, tx_ms);

            if (ul_ms < -5000 || ul_ms > 30000) continue;
            if (dl_ms < -5000 || dl_ms > 30000) continue;

            int64_t ul_owd_us = (int64_t)ul_ms * 1000LL;
            int64_t dl_owd_us = (int64_t)dl_ms * 1000LL;

            /* Consume slot so stale replies don't double-count */
            slot->t_sent_us    = 0;
            slot->reflector_idx = -1;

            process_owd(ar, ridx, dl_owd_us, ul_owd_us, t_now_us);
        }
    }
}

/* ────────────────────────────────────────────────────────────── */
/*  Ping timer callback – send one ping (echo or timestamp)       */
/* ────────────────────────────────────────────────────────────── */
static void ping_timer_cb(struct uloop_timeout *t)
{
    autorate_t    *ar = container_of(t, autorate_t, ping_timer);
    cake_config_t *c  = &ar->cfg;

    if (ar->no_active_reflectors <= 0 || ar->icmp_sock < 0) {
        uloop_timeout_set(&ar->ping_timer, 1000);
        return;
    }

    if (ar->ping_rr_idx >= ar->no_active_reflectors)
        ar->ping_rr_idx = 0;

    int ridx = ar->ping_rr_idx++;
    reflector_t *r = &ar->reflectors[ridx];

    if (r->addr_be == 0) goto out;

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_addr.s_addr = r->addr_be;

    uint16_t seq = ++ar->ping_seq;
    int64_t  t_sent_us = now_us();

    if (c->ping_type == 1) {
        /* ── ICMP Timestamp Request (type 13) ────────────────── */
        uint8_t pkt[sizeof(struct icmphdr) + sizeof(struct icmp_ts_body)];
        memset(pkt, 0, sizeof(pkt));

        struct icmphdr      *h   = (struct icmphdr *)pkt;
        struct icmp_ts_body *tsb = (struct icmp_ts_body *)(h + 1);

        h->type               = ICMP_TIMESTAMP;
        h->code               = 0;
        h->un.echo.id         = htons(ar->ping_id);
        h->un.echo.sequence   = htons(seq);

        uint32_t orig_ms = ms_since_midnight_realtime();
        tsb->originate = htonl(orig_ms);
        tsb->receive   = 0;
        tsb->transmit  = 0;

        h->checksum = 0;
        h->checksum = csum16(pkt, sizeof(pkt));

        /* Store in ring for reply correlation */
        ping_seq_slot_t *slot = &ar->ping_seq_ring[seq % PING_SEQ_RING];
        slot->t_sent_us     = t_sent_us;
        slot->originate_ms  = orig_ms;
        slot->reflector_idx = ridx;

        (void)sendto(ar->icmp_sock, pkt, sizeof(pkt), 0,
                     (struct sockaddr *)&dst, sizeof(dst));

    } else {
        /* ── ICMP Echo Request (type 8) ──────────────────────── */
        uint8_t pkt[sizeof(struct icmphdr) + sizeof(struct ping_payload)];
        memset(pkt, 0, sizeof(pkt));

        struct icmphdr      *h  = (struct icmphdr *)pkt;
        struct ping_payload *pl = (struct ping_payload *)(h + 1);

        h->type             = ICMP_ECHO;
        h->code             = 0;
        h->un.echo.id       = htons(ar->ping_id);
        h->un.echo.sequence = htons(seq);

        pl->magic_be    = htonl(PING_PAYLOAD_MAGIC);
        pl->ridx_be     = htons((uint16_t)ridx);
        pl->reserved_be = 0;
        write_be64(pl->t_sent_be64, (uint64_t)t_sent_us);

        h->checksum = 0;
        h->checksum = csum16(pkt, sizeof(pkt));

        (void)sendto(ar->icmp_sock, pkt, sizeof(pkt), 0,
                     (struct sockaddr *)&dst, sizeof(dst));
    }

out:
    {
        int interval_ms = (int)((c->reflector_ping_interval_us / 1000)
                                / ar->no_active_reflectors);
        if (interval_ms < 10) interval_ms = 10;
        uloop_timeout_set(&ar->ping_timer, interval_ms);
    }
}

static void refresh_reflector_addrs(autorate_t *ar)
{
    for (int i = 0; i < ar->no_active_reflectors; i++) {
        struct in_addr a;
        if (inet_pton(AF_INET, ar->reflectors[i].addr, &a) == 1)
            ar->reflectors[i].addr_be = a.s_addr;
        else
            ar->reflectors[i].addr_be = 0;
    }
}

static int start_pinger(autorate_t *ar)
{
    ar->icmp_sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (ar->icmp_sock < 0)
        return -1;

    /*
     * Optional device binding (multi-WAN).  Without it, policy routing
     * (mwan3) may steer reflector pings out an arbitrary WAN, corrupting
     * the per-instance OWD measurement.  SO_BINDTODEVICE pins both the
     * outgoing pings and the accepted replies to this interface.
     */
    if (ar->cfg.ping_bind_if[0]) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ar->cfg.ping_bind_if);
        if (setsockopt(ar->icmp_sock, SOL_SOCKET, SO_BINDTODEVICE,
                       &ifr, sizeof(ifr)) < 0) {
            syslog(LOG_ERR, "ping bind to '%s': %m", ar->cfg.ping_bind_if);
            close(ar->icmp_sock);
            ar->icmp_sock = -1;
            return -1;
        }
    }

    set_nonblocking(ar->icmp_sock);

    ar->icmp_ufd.fd = ar->icmp_sock;
    ar->icmp_ufd.cb = icmp_reply_cb;
    uloop_fd_add(&ar->icmp_ufd, ULOOP_READ | ULOOP_EDGE_TRIGGER);

    /* Use getpid() only — PIDs are unique per instance, so XOR-ing
     * with time(NULL) only introduces collision risk on fast reboots. */
    ar->ping_id     = (uint16_t)(getpid() & 0xFFFF);
    ar->ping_seq    = 0;
    ar->ping_rr_idx = 0;

    /* Mark all ring slots as unused */
    for (int i = 0; i < PING_SEQ_RING; i++) {
        ar->ping_seq_ring[i].t_sent_us     = 0;
        ar->ping_seq_ring[i].reflector_idx = -1;
    }

    refresh_reflector_addrs(ar);

    ar->ping_timer.cb = ping_timer_cb;
    uloop_timeout_set(&ar->ping_timer, 1);
    return 0;
}

static void stop_pinger(autorate_t *ar)
{
    uloop_timeout_cancel(&ar->ping_timer);

    if (ar->icmp_ufd.registered)
        uloop_fd_delete(&ar->icmp_ufd);

    if (ar->icmp_sock >= 0) {
        close(ar->icmp_sock);
        ar->icmp_sock = -1;
    }
}

/* ────────────────────────────────────────────────────────────── */
/*  Reflector health check timer                                  */
/* ────────────────────────────────────────────────────────────── */
static void health_timer_cb(struct uloop_timeout *t)
{
    autorate_t    *ar  = container_of(t, autorate_t, health_timer);
    cake_config_t *c   = &ar->cfg;
    int64_t        now = now_us();
    int            win = c->reflector_misbehaving_detection_window;
    int            replaced = 0;

    if (win <= 0 || win > MAX_OFFENCE_WINDOW)
        win = MAX_OFFENCE_WINDOW;

    for (int i = 0; i < ar->no_active_reflectors; i++) {
        reflector_t *r = &ar->reflectors[i];

        if (r->last_response_us == 0)
            continue;

        int offence = (now - r->last_response_us >
                       c->reflector_response_deadline_us) ? 1 : 0;

        int widx = r->offences_idx;
        r->sum_offences  -= r->offences[widx];
        r->offences[widx] = offence;
        r->sum_offences  += offence;
        r->offences_idx   = (widx + 1) % win;

        if (r->sum_offences >= c->reflector_misbehaving_detection_thr &&
            ar->spare_idx   <  c->no_reflectors) {

            syslog(LOG_WARNING,
                   "replacing misbehaving reflector %s with %s "
                   "(%d/%d misses in window)",
                   r->addr, c->reflectors[ar->spare_idx],
                   r->sum_offences, win);

            snprintf(r->addr, sizeof(r->addr),
                     "%s", c->reflectors[ar->spare_idx++]);

            /*
             * Warm-start the new reflector's baseline using the median of
             * the surviving active reflectors rather than zero.
             *
             * Setting baseline = 0 causes the first real sample to become
             * the baseline immediately (the == 0 guard in process_owd).
             * That single sample may be an outlier, and with
             * alpha_baseline_increase = 0.001 the baseline barely moves
             * upward afterwards.  If the new reflector's natural latency
             * is higher than the first sample, the baseline undershoots for
             * many minutes, making deltas look artificially elevated and
             * triggering false bufferbloat detections that incorrectly
             * reduce shaper rates — which is the "bufferbloat worsens over
             * time until restart" symptom.
             *
             * Using the median of healthy reflectors gives a reasonable
             * starting point that is in the right ballpark regardless of
             * the new reflector's clock offset (the EWMA will correct the
             * remainder within a few seconds at alpha_decrease = 0.9).
             */
            {
                int64_t dl_vals[MAX_REFLECTORS];
                int64_t ul_vals[MAX_REFLECTORS];
                int n = 0;
                for (int j = 0; j < ar->no_active_reflectors; j++) {
                    if (j == i) continue;
                    if (ar->reflectors[j].dl_owd_baseline_us > 0) {
                        dl_vals[n] = ar->reflectors[j].dl_owd_baseline_us;
                        ul_vals[n] = ar->reflectors[j].ul_owd_baseline_us;
                        n++;
                    }
                }
                if (n > 0) {
                    /* Simple selection sort for small n (≤ 20) */
                    for (int a = 0; a < n - 1; a++) {
                        for (int b = a + 1; b < n; b++) {
                            if (dl_vals[b] < dl_vals[a]) {
                                int64_t tmp = dl_vals[a];
                                dl_vals[a] = dl_vals[b];
                                dl_vals[b] = tmp;
                                tmp = ul_vals[a];
                                ul_vals[a] = ul_vals[b];
                                ul_vals[b] = tmp;
                            }
                        }
                    }
                    r->dl_owd_baseline_us = dl_vals[n / 2];
                    r->ul_owd_baseline_us = ul_vals[n / 2];
                } else {
                    r->dl_owd_baseline_us = 0;
                    r->ul_owd_baseline_us = 0;
                }
            }

            r->dl_owd_delta_ewma_us = 0;
            r->ul_owd_delta_ewma_us = 0;
            r->last_response_us     = 0;
            memset(r->offences, 0, sizeof(r->offences));
            r->sum_offences = 0;
            r->offences_idx = 0;
            replaced = 1;
        }
    }

    if (replaced)
        refresh_reflector_addrs(ar);

    uloop_timeout_set(t, (int)(c->reflector_health_check_interval_us / 1000));
}

/* ────────────────────────────────────────────────────────────── */
/*  Rate-monitor timer callback (~200 ms)                         */
/* ────────────────────────────────────────────────────────────── */
static void rate_timer_cb(struct uloop_timeout *t)
{
    autorate_t    *ar = container_of(t, autorate_t, rate_timer);
    cake_config_t *c  = &ar->cfg;

    int64_t elapsed = rate_monitor_update(&ar->rm,
                                          &ar->achieved_rate_kbps[DIR_DL],
                                          &ar->achieved_rate_kbps[DIR_UL]);

    ar->achieved_rate_updated[DIR_DL] = 1;
    ar->achieved_rate_updated[DIR_UL] = 1;

    /* Drift compensation */
    int64_t target = c->monitor_achieved_rates_interval_us;
    int64_t next   = target - (elapsed - target);
    if (next < target) next = target;

    /* Stall detection
     *
     * The threshold must be based on the per-reflector ping interval, not
     * the global interval divided by reflector count.  Each reflector is
     * pinged every reflector_ping_interval_s seconds; if none of them have
     * replied for (stall_detection_thr × per-reflector interval) we declare
     * a stall.  Using the short global interval caused false stalls whenever
     * a single reflector missed one response.
     *
     * We also require the rate check to persist for two consecutive monitor
     * intervals before logging, which suppresses single-sample glitches.
     */
    int64_t now = now_us();
    {
        int64_t per_reflector_interval_us = c->reflector_ping_interval_us;
        int64_t stall_thr_us =
            (int64_t)c->stall_detection_thr * per_reflector_interval_us;

        int64_t last_response = ar->global_last_response_us > 0
            ? ar->global_last_response_us
            : ar->t_started_us;

        if (now - last_response > stall_thr_us &&
            ar->achieved_rate_kbps[DIR_DL] < c->connection_stall_thr_kbps &&
            ar->achieved_rate_kbps[DIR_UL] < c->connection_stall_thr_kbps) {
            if (ar->main_state != STATE_STALL) {
                ar->main_state = STATE_STALL;
                if (ar->global_last_response_us == 0)
                    syslog(LOG_WARNING,
                           "no ping responses received since startup "
                           "(ping_type=%d) – reflectors may not support "
                           "this ICMP type; check reflector list or set "
                           "ping_type=0 for ICMP Echo",
                           c->ping_type);
                else
                    syslog(LOG_WARNING, "connection stall detected");
            }
        } else if (ar->main_state == STATE_STALL &&
                   ar->global_last_response_us > 0) {
            ar->main_state = STATE_RUNNING;
            syslog(LOG_INFO, "connection recovered from stall");
        }
    }

    /* Update LuCI status file every rate-monitor tick */
    write_status_file(ar);

    int ms = (int)((next / 1000) & 0x7FFFFFFF);
    uloop_timeout_set(t, ms);
}

/* ────────────────────────────────────────────────────────────── */
/*  Sliding-window allocation                                     */
/* ────────────────────────────────────────────────────────────── */
static int init_windows(autorate_t *ar)
{
    int w = ar->cfg.bufferbloat_detection_window;
    ar->dl_delays        = calloc((size_t)w, sizeof(int));
    ar->ul_delays        = calloc((size_t)w, sizeof(int));
    ar->dl_owd_deltas_us = calloc((size_t)w, sizeof(int64_t));
    ar->ul_owd_deltas_us = calloc((size_t)w, sizeof(int64_t));
    return (ar->dl_delays && ar->ul_delays &&
            ar->dl_owd_deltas_us && ar->ul_owd_deltas_us) ? 0 : -1;
}

/* ────────────────────────────────────────────────────────────── */
/*  Signal handler                                                */
/* ────────────────────────────────────────────────────────────── */
static void handle_signal(int sig)
{
    (void)sig;
    uloop_end();
}

/* ────────────────────────────────────────────────────────────── */
/*  Interface up/down recovery (if_up_check_interval_us)          */
/* ────────────────────────────────────────────────────────────── */
/*
 * if_up_timer_cb – polls for WAN interface presence every
 * if_up_check_interval_us microseconds.
 *
 * On PPPoE/DHCP reconnects the WAN interface disappears and reappears.
 * While it is absent we pause pinging and rate adjustments so the daemon
 * does not burn through all spare reflectors or drive rates to their
 * minimums on a link that is simply renegotiating.
 *
 * When the interface comes back we re-run cake_setup() to recreate the
 * CAKE qdiscs and restart the pinger from scratch.
 */
static void if_up_timer_cb(struct uloop_timeout *t)
{
    autorate_t    *ar = container_of(t, autorate_t, if_up_timer);
    cake_config_t *c  = &ar->cfg;

    int iface_present = (if_nametoindex(c->ul_if) != 0);

    if (!iface_present && ar->link_up) {
        /* Interface just disappeared */
        ar->link_up = 0;
        syslog(LOG_WARNING,
               "WAN interface '%s' disappeared – pausing rate control",
               c->ul_if);

        /* Stop pinger so we don't flood with unanswerable pings */
        stop_pinger(ar);

        /* Tear down CAKE qdiscs; they will be recreated on recovery */
        cake_teardown(ar);

        /* Reset shaper rates to base so we start fresh on reconnect */
        ar->shaper_rate_kbps[DIR_DL] = c->base_dl_shaper_rate_kbps;
        ar->shaper_rate_kbps[DIR_UL] = c->base_ul_shaper_rate_kbps;

    } else if (iface_present && !ar->link_up) {
        /* Interface just came back up */
        syslog(LOG_INFO,
               "WAN interface '%s' reappeared – resuming rate control",
               c->ul_if);

        if (cake_setup(ar) < 0) {
            syslog(LOG_ERR,
                   "if_up: CAKE re-setup failed on '%s', will retry",
                   c->ul_if);
            /* Don't flip link_up; retry next interval */
        } else {
            ar->link_up = 1;

            /* Reset OWD state so the new link gets a fresh baseline */
            int bdw = c->bufferbloat_detection_window;
            memset(ar->dl_delays,        0, (size_t)bdw * sizeof(*ar->dl_delays));
            memset(ar->ul_delays,        0, (size_t)bdw * sizeof(*ar->ul_delays));
            memset(ar->dl_owd_deltas_us, 0, (size_t)bdw * sizeof(*ar->dl_owd_deltas_us));
            memset(ar->ul_owd_deltas_us, 0, (size_t)bdw * sizeof(*ar->ul_owd_deltas_us));
            ar->delays_idx             = 0;
            ar->delays_fill            = 0;
            ar->sum_dl_delays          = 0;
            ar->sum_ul_delays          = 0;
            ar->sum_dl_owd_deltas_us   = 0;
            ar->sum_ul_owd_deltas_us   = 0;
            ar->global_last_response_us = 0;

            for (int i = 0; i < ar->no_active_reflectors; i++) {
                reflector_t *r = &ar->reflectors[i];
                r->dl_owd_baseline_us   = 0;
                r->ul_owd_baseline_us   = 0;
                r->dl_owd_delta_ewma_us = 0;
                r->ul_owd_delta_ewma_us = 0;
                r->last_response_us     = 0;
                memset(r->offences, 0, sizeof(r->offences));
                r->sum_offences = 0;
                r->offences_idx = 0;
            }

            if (start_pinger(ar) < 0)
                syslog(LOG_ERR, "if_up: failed to restart pinger: %m");
        }

    } else if (iface_present && ar->link_up && !ar->ul_setup_done &&
               c->adjust_ul_shaper_rate && c->ul_if[0]) {
        cake_qdisc_opts_t ul_opts = make_ul_opts(c);
        if (tc_ul_setup(ar->tc_nl, c->ul_if,
                        ar->shaper_rate_kbps[DIR_UL], &ul_opts) == 0) {
            ar->ul_setup_done = 1;
            ar->last_shaper_rate_kbps[DIR_UL] = ar->shaper_rate_kbps[DIR_UL];
            syslog(LOG_INFO, "if_up: UL path ready on '%s'", c->ul_if);
        }
    }

    int interval_ms = (int)(c->if_up_check_interval_us / 1000);
    if (interval_ms < 1000) interval_ms = 1000;  /* minimum 1 s */
    uloop_timeout_set(t, interval_ms);
}

/* ────────────────────────────────────────────────────────────── */
/*  Daemon entry point – dynamic (adaptive) instances             */
/* ────────────────────────────────────────────────────────────── */
static int daemon_run(const char *section)
{
    autorate_t ar;
    memset(&ar, 0, sizeof(ar));
    ar.icmp_sock  = -1;
    ar.rm.rx_fd   = -1;
    ar.rm.tx_fd   = -1;

    /* ── Load configuration ──────────────────────────────────── */
    if (config_load(section, &ar.cfg) < 0) {
        fprintf(stderr, "antilag: failed to load UCI config '%s'\n",
                section);
        syslog(LOG_ERR, "failed to load UCI config section '%s'", section);
        return 1;
    }

    if (!ar.cfg.enabled) {
        syslog(LOG_INFO, "instance '%s' disabled, exiting", section);
        return 0;
    }

    /*
     * Static instances do not run a daemon: install the fixed-rate qdiscs
     * now and exit.  This keeps the legacy `antilag <section>` invocation
     * working if an old init script is still in place.
     */
    if (ar.cfg.mode == MODE_STATIC)
        return static_apply(section);

    /* ── Per-instance status file paths ──────────────────────── */
    if (!instance_id_valid(ar.cfg.instance_id)) {
        syslog(LOG_ERR, "invalid section name '%s' – use [A-Za-z0-9_-] only",
               ar.cfg.instance_id);
        return 1;
    }
    snprintf(ar.status_path, sizeof(ar.status_path),
             "/var/run/antilag-%s.json", ar.cfg.instance_id);
    snprintf(ar.status_tmp_path, sizeof(ar.status_tmp_path),
             "/var/run/antilag-%s.json.tmp", ar.cfg.instance_id);

    if (ar.cfg.no_pingers < 1) {
        syslog(LOG_ERR, "no_pingers must be >= 1");
        return 1;
    }

    /* ── Allocate OWD sliding windows ────────────────────────── */
    if (init_windows(&ar) < 0) {
        syslog(LOG_ERR, "out of memory allocating OWD windows");
        return 1;
    }

    /* Active reflectors = first min(no_pingers, no_reflectors) entries. */
    ar.no_active_reflectors =
        (ar.cfg.no_pingers < ar.cfg.no_reflectors)
        ? ar.cfg.no_pingers
        : ar.cfg.no_reflectors;
    ar.spare_idx = ar.no_active_reflectors;

    for (int i = 0; i < ar.no_active_reflectors; i++)
        snprintf(ar.reflectors[i].addr, 64, "%s", ar.cfg.reflectors[i]);

    /* Initial shaper rates */
    ar.shaper_rate_kbps[DIR_DL] = ar.cfg.base_dl_shaper_rate_kbps;
    ar.shaper_rate_kbps[DIR_UL] = ar.cfg.base_ul_shaper_rate_kbps;
    ar.link_up = 1;  /* assume up at start; if_up_timer will correct if wrong */

    /* Status file for LuCI – written every rate-monitor tick */
    ar.t_started_us = now_us();

    ar.ping_response_interval_us =
        ar.cfg.reflector_ping_interval_us / ar.no_active_reflectors;

    /* ── Open netlink socket ─────────────────────────────────── */
    ar.tc_nl = tc_nl_open();
    if (!ar.tc_nl) {
        syslog(LOG_ERR, "tc_netlink: failed to open netlink socket");
        goto err_free_windows;
    }

    /* ── Create CAKE qdiscs (replaces SQM script) ────────────── */
    if (cake_setup(&ar) < 0) {
        syslog(LOG_ERR, "CAKE setup failed – check interface names and permissions");
        goto err_teardown;  /* runs cake_teardown() to clean up partial DL setup */
    }

    /* ── Startup wait ────────────────────────────────────────── */
    if (ar.cfg.startup_wait_us > 0)
        usleep((unsigned int)ar.cfg.startup_wait_us);

    /* ── Initialise rate monitor ─────────────────────────────── */
    rate_monitor_init(&ar.rm, ar.cfg.dl_if, ar.cfg.ul_if);

    /* ── uloop event loop ────────────────────────────────────── */
    uloop_init();
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    if (start_pinger(&ar) < 0) {
        syslog(LOG_ERR, "failed to start integrated pinger (raw ICMP)");
        goto err_teardown;
    }

    ar.rate_timer.cb = rate_timer_cb;
    uloop_timeout_set(&ar.rate_timer,
        (int)(ar.cfg.monitor_achieved_rates_interval_us / 1000));

    ar.health_timer.cb = health_timer_cb;
    uloop_timeout_set(&ar.health_timer,
        (int)(ar.cfg.reflector_health_check_interval_us / 1000));

    /* Feature: interface recovery – only arm if interval is configured */
    if (ar.cfg.if_up_check_interval_us > 0) {
        ar.if_up_timer.cb = if_up_timer_cb;
        int if_up_ms = (int)(ar.cfg.if_up_check_interval_us / 1000);
        if (if_up_ms < 1000) if_up_ms = 1000;
        uloop_timeout_set(&ar.if_up_timer, if_up_ms);
    }

    syslog(LOG_INFO, "started instance '%s' dl=%s ul=%s ping_type=%s ping_bind=%s",
           section, ar.cfg.dl_if, ar.cfg.ul_if,
           ar.cfg.ping_type == 1 ? "ICMP-timestamp(13)" : "ICMP-echo(8)",
           ar.cfg.ping_bind_if[0] ? ar.cfg.ping_bind_if : "(unbound)");

    uloop_run();
    uloop_done();

    /* ── Graceful shutdown ───────────────────────────────────── */
    syslog(LOG_INFO, "shutting down instance '%s'", section);

    uloop_timeout_cancel(&ar.rate_timer);
    uloop_timeout_cancel(&ar.health_timer);
    uloop_timeout_cancel(&ar.if_up_timer);
    stop_pinger(&ar);

    /* Remove status file so LuCI shows the service as stopped */
    unlink(ar.status_path);

err_teardown:
    /*
     * Remove all CAKE TC objects we created.
     * This leaves the interfaces clean (no rate limiting) after exit.
     */
    cake_teardown(&ar);

err_tc_close:
    tc_nl_close(ar.tc_nl);
    ar.tc_nl = NULL;

    rate_monitor_cleanup(&ar.rm);

err_free_windows:
    free(ar.dl_delays);
    free(ar.ul_delays);
    free(ar.dl_owd_deltas_us);
    free(ar.ul_owd_deltas_us);

    return 0;
}

/* ────────────────────────────────────────────────────────────── */
/*  main – subcommand dispatch                                    */
/*                                                                */
/*    antilag run   <section>   run the adaptive daemon           */
/*    antilag apply <section>   one-shot install of a static      */
/*                              instance (init + iface hotplug)   */
/*    antilag stop  <section>   tear down an instance's qdiscs    */
/*    antilag <section>         legacy alias for `run`            */
/* ────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    const char *action  = "run";
    const char *section = "wan";
    int         rc;

    openlog("antilag", LOG_PID | LOG_NDELAY, LOG_DAEMON);

    if (argc > 1 &&
        (strcmp(argv[1], "run")   == 0 ||
         strcmp(argv[1], "apply") == 0 ||
         strcmp(argv[1], "stop")  == 0)) {
        action = argv[1];
        if (argc > 2 && argv[2][0])
            section = argv[2];
    } else if (argc > 1 && argv[1][0]) {
        section = argv[1];          /* legacy: `antilag <section>` */
    }

    if (strcmp(action, "apply") == 0)
        rc = static_apply(section);
    else if (strcmp(action, "stop") == 0)
        rc = instance_stop(section);
    else
        rc = daemon_run(section);

    closelog();
    return rc;
}
