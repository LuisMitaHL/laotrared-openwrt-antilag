/*
 * tc_netlink.c  –  Standalone CAKE qdisc management via raw NETLINK_ROUTE
 *
 * Replaces the SQM scripts dependency with in-process netlink calls:
 *
 *   IFB management   –  RTM_NEWLINK / RTM_DELLINK
 *   Qdisc management –  RTM_NEWQDISC / RTM_DELQDISC
 *   Filter/action    –  RTM_NEWTFILTER (u32 match-all + tc_mirred redirect)
 *
 * All messages use the same persistent NETLINK_ROUTE socket opened at
 * startup.  No fork, no exec, no shell, no dependency on tc or ip binaries.
 *
 * References:
 *   iproute2 tc/q_cake.c, tc/f_u32.c, tc/m_mirred.c, ip/link.c
 *   linux/net/sched/sch_cake.c
 *   linux/net/sched/cls_u32.c
 *   linux/net/sched/act_mirred.c
 */

#include "tc_netlink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>
#include <limits.h>

#include <net/if.h>                   /* if_nametoindex, IFF_UP          */
#include <sys/socket.h>
#include <arpa/inet.h>                /* htons                            */

#include <linux/netlink.h>            /* NETLINK_ROUTE, nlmsghdr          */
#include <linux/rtnetlink.h>          /* RTM_*, tcmsg, ifinfomsg, TCA_*   */
#include <linux/if_link.h>            /* IFLA_IFNAME, IFLA_LINKINFO, …    */
#include <linux/pkt_sched.h>          /* TC_H_ROOT, TC_H_INGRESS, TCA_*   */
#include <linux/tc_act/tc_mirred.h>   /* TCA_MIRRED_PARMS, TCA_EGRESS_REDIR */
#include <linux/if_ether.h>           /* ETH_P_ALL                        */

/* ── CAKE attribute guards ───────────────────────────────────────────── */

#ifndef TCA_CAKE_BASE_RATE64
#define TCA_CAKE_BASE_RATE64    2
#endif
#ifndef TCA_CAKE_DIFFSERV_MODE
#define TCA_CAKE_DIFFSERV_MODE  3
#define TCA_CAKE_ATM            4
#define TCA_CAKE_FLOW_MODE      5
#define TCA_CAKE_OVERHEAD       6
#define TCA_CAKE_RTT            7
#define TCA_CAKE_TARGET         8
#define TCA_CAKE_AUTORATE_ING   9
#define TCA_CAKE_MEMORY         10
#define TCA_CAKE_NAT            11
#define TCA_CAKE_WASH           13
#define TCA_CAKE_MPU            14
#define TCA_CAKE_INGRESS        15
#define TCA_CAKE_ACK_FILTER     16
#define TCA_CAKE_SPLIT_GSO      17
#endif

/* TC_H_INGRESS might not be in older linux/pkt_sched.h snapshots */
#ifndef TC_H_INGRESS
#define TC_H_INGRESS  0xFFFFFFF1U
#endif

/* TC_ACT_* may not be present in pkt_cls.h on very old headers */
#ifndef TC_ACT_STOLEN
#define TC_ACT_STOLEN  4
#endif

/* TCA_EGRESS_REDIR: mirred action – redirect to egress of another interface */
#ifndef TCA_EGRESS_REDIR
#define TCA_EGRESS_REDIR  1
#endif

/*
 * TCA_MATCHALL_ACT – the only matchall option we use.
 * matchall is purpose-built for "match every packet"; unlike u32 it has no
 * hash-table lookup and does not inspect packet data at all.
 * Value 2 is stable since kernel 4.12 (linux/pkt_cls.h).
 */
#ifndef TCA_MATCHALL_ACT
#define TCA_MATCHALL_ACT  2
#endif

/* ── Internal state ──────────────────────────────────────────────────── */

/*
 * Cache the qdisc opts per interface so that the emergency
 * fallback "add" path in tc_cake_set_bandwidth() can recreate the qdisc
 * with all CAKE options preserved (diffserv, flow mode, NAT, wash, etc.)
 * rather than a bare bandwidth-only qdisc.
 *
 * We store up to NL_CACHED_IFACES (ifindex, opts) pairs.  In practice
 * the daemon uses at most 2 (DL IFB + UL WAN).
 */
#define NL_CACHED_IFACES 4

struct tc_nl_ctx {
    int      fd;
    uint32_t seq;

    /* Per-interface opts cache (populated by tc_dl_setup / tc_ul_setup) */
    unsigned int      cached_ifindex[NL_CACHED_IFACES];
    cake_qdisc_opts_t cached_opts[NL_CACHED_IFACES];
    int               cached_count;
};

/* ── Buffer sizes ────────────────────────────────────────────────────── */

/*
 * 1024 bytes is generous for all message types we send.
 * Largest is the u32 mirred filter (~400 bytes incl. all nesting).
 */
#define NL_BUF_SIZE   1024
#define NL_RECV_SIZE   512

/* ── Low-level nlattr helpers ────────────────────────────────────────── */

/*
 * nla_stripped_type – attribute type without the NLA_F_NESTED flag.
 * The kernel sets 0x8000 on every nested attribute it sends
 * (TCA_STATS2, TCA_STATS_APP, TCA_CAKE_STATS_TIN_STATS, per-tin
 * nests), so raw comparisons against TCA_* constants never match
 * on the parse path. iproute2 masks the same way.
 */
static inline uint16_t nla_stripped_type(const struct nlattr *a)
{
    return (uint16_t)(a->nla_type & NLA_TYPE_MASK);
}

static void nl_put_attr(char *buf, int *pos,
                        uint16_t type, const void *data, uint16_t dlen)
{
    struct nlattr *nla = (struct nlattr *)(buf + *pos);
    nla->nla_type = type;
    nla->nla_len  = (uint16_t)(NLA_HDRLEN + dlen);
    if (dlen && data)
        memcpy((char *)nla + NLA_HDRLEN, data, dlen);
    int pad = (int)NLA_ALIGN(nla->nla_len) - nla->nla_len;
    if (pad > 0)
        memset((char *)nla + nla->nla_len, 0, pad);
    *pos += (int)NLA_ALIGN(nla->nla_len);
}

static void nl_put_u32(char *buf, int *pos, uint16_t type, uint32_t val)
{
    nl_put_attr(buf, pos, type, &val, sizeof(val));
}

static void nl_put_s32(char *buf, int *pos, uint16_t type, int32_t val)
{
    nl_put_attr(buf, pos, type, &val, sizeof(val));
}

static void nl_put_u64(char *buf, int *pos, uint16_t type, uint64_t val)
{
    nl_put_attr(buf, pos, type, &val, sizeof(val));
}

static void nl_put_str(char *buf, int *pos, uint16_t type, const char *s)
{
    nl_put_attr(buf, pos, type, s, (uint16_t)(strlen(s) + 1));
}

static int nl_nest_start(char *buf, int *pos, uint16_t type)
{
    int off = *pos;
    struct nlattr *nla = (struct nlattr *)(buf + off);
    nla->nla_type = type | NLA_F_NESTED;
    nla->nla_len  = NLA_HDRLEN;
    *pos += NLA_HDRLEN;
    return off;
}

static void nl_nest_end(char *buf, int *pos, int nest_off)
{
    struct nlattr *nla = (struct nlattr *)(buf + nest_off);
    nla->nla_len = (uint16_t)(*pos - nest_off);
}

/* ── Core transact helper ────────────────────────────────────────────── */

/*
 * nl_transact – patch message length + seq, send, receive ACK.
 *
 * Returns  0       success
 *         -errno   kernel error (errno also set)
 *         -1       local send/recv error (errno set)
 */
static int nl_transact(tc_nl_ctx_t *ctx, char *buf, int msg_len)
{
    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    nlh->nlmsg_len = (uint32_t)msg_len;
    nlh->nlmsg_seq = ctx->seq++;
    nlh->nlmsg_pid = 0;

    struct sockaddr_nl dst;
    memset(&dst, 0, sizeof(dst));
    dst.nl_family = AF_NETLINK;

    if (sendto(ctx->fd, buf, (size_t)msg_len, 0,
               (struct sockaddr *)&dst, sizeof(dst)) < 0)
        return -1;

    /*
     * Loop until we receive the ACK for *our* sequence number.
     * An unsolicited netlink notification (link-up/down event, kernel
     * route change) arriving before the ACK would otherwise be silently
     * parsed as our response, corrupting the return value.
     */
    uint32_t sent_seq = nlh->nlmsg_seq;
    char rbuf[NL_RECV_SIZE];
    for (;;) {
        ssize_t n = recv(ctx->fd, rbuf, sizeof(rbuf), 0);
        if (n < (ssize_t)NLMSG_HDRLEN)
            return -1;

        struct nlmsghdr *ack = (struct nlmsghdr *)rbuf;

        /* Discard any message that isn't our ACK */
        if (ack->nlmsg_seq != sent_seq)
            continue;

        if (ack->nlmsg_type != NLMSG_ERROR)
            return -1;
        if ((size_t)n < NLMSG_HDRLEN + sizeof(struct nlmsgerr))
            return -1;

        struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(ack);
        if (err->error != 0) {
            errno = -err->error;
            return err->error;   /* negative errno */
        }
        return 0;
    }
}

/* ── Socket lifecycle ────────────────────────────────────────────────── */

/* Store opts for an interface, replacing any existing entry for that ifindex */
static void tc__cache_opts(tc_nl_ctx_t *ctx, unsigned int ifindex,
                            const cake_qdisc_opts_t *opts)
{
    if (!opts) return;

    /* Update existing entry if present */
    for (int i = 0; i < ctx->cached_count; i++) {
        if (ctx->cached_ifindex[i] == ifindex) {
            ctx->cached_opts[i] = *opts;
            return;
        }
    }

    /* Add new entry if there is room */
    if (ctx->cached_count < NL_CACHED_IFACES) {
        ctx->cached_ifindex[ctx->cached_count] = ifindex;
        ctx->cached_opts[ctx->cached_count]    = *opts;
        ctx->cached_count++;
    }
}

/* Look up cached opts for an ifindex; returns NULL if not found */
static const cake_qdisc_opts_t *tc__lookup_opts(const tc_nl_ctx_t *ctx,
                                                  unsigned int ifindex)
{
    for (int i = 0; i < ctx->cached_count; i++) {
        if (ctx->cached_ifindex[i] == ifindex)
            return &ctx->cached_opts[i];
    }
    return NULL;
}

tc_nl_ctx_t *tc_nl_open(void)
{
    tc_nl_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (ctx->fd < 0) {
        syslog(LOG_ERR, "tc_netlink: socket(): %m");
        free(ctx);
        return NULL;
    }

    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;

    if (bind(ctx->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        syslog(LOG_ERR, "tc_netlink: bind(): %m");
        close(ctx->fd);
        free(ctx);
        return NULL;
    }

    ctx->seq = 1;
    return ctx;
}

void tc_nl_close(tc_nl_ctx_t *ctx)
{
    if (!ctx)
        return;
    close(ctx->fd);
    free(ctx);
}

/* ════════════════════════════════════════════════════════════════════════
 * Link management helpers (IFB create / set-up / delete)
 * ════════════════════════════════════════════════════════════════════════ */

/*
 * tc__link_create_ifb  –  RTM_NEWLINK + NLM_F_CREATE|NLM_F_EXCL
 *
 * Equivalent to:  ip link add <name> type ifb
 * Returns 0 on success, -EEXIST if already present, other -errno on error.
 */
static int tc__link_create_ifb(tc_nl_ctx_t *ctx, const char *name)
{
    char buf[NL_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    nlh->nlmsg_type  = RTM_NEWLINK;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;

    int pos = NLMSG_HDRLEN;

    struct ifinfomsg *ifi = (struct ifinfomsg *)(buf + pos);
    ifi->ifi_family = AF_UNSPEC;
    pos += (int)NLMSG_ALIGN(sizeof(*ifi));

    nl_put_str(buf, &pos, IFLA_IFNAME, name);

    /* IFLA_LINKINFO { IFLA_INFO_KIND = "ifb" } */
    int li = nl_nest_start(buf, &pos, IFLA_LINKINFO);
    nl_put_str(buf, &pos, IFLA_INFO_KIND, "ifb");
    nl_nest_end(buf, &pos, li);

    return nl_transact(ctx, buf, pos);
}

/*
 * tc__link_set_up  –  bring an interface UP via RTM_NEWLINK.
 *
 * Equivalent to:  ip link set <name> up
 */
static int tc__link_set_up(tc_nl_ctx_t *ctx, const char *name)
{
    unsigned int ifindex = if_nametoindex(name);
    if (!ifindex) {
        errno = ENODEV;
        return -1;
    }

    char buf[NL_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    nlh->nlmsg_type  = RTM_NEWLINK;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;

    int pos = NLMSG_HDRLEN;

    struct ifinfomsg *ifi = (struct ifinfomsg *)(buf + pos);
    ifi->ifi_family  = AF_UNSPEC;
    ifi->ifi_index   = (int)ifindex;
    ifi->ifi_flags   = IFF_UP;
    ifi->ifi_change  = IFF_UP;
    pos += (int)NLMSG_ALIGN(sizeof(*ifi));

    return nl_transact(ctx, buf, pos);
}

/*
 * tc__link_delete  –  RTM_DELLINK.
 *
 * Equivalent to:  ip link del <name>
 * Silently returns 0 if the interface does not exist.
 */
static int tc__link_delete(tc_nl_ctx_t *ctx, const char *name)
{
    unsigned int ifindex = if_nametoindex(name);
    if (!ifindex)
        return 0;   /* already gone */

    char buf[NL_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    nlh->nlmsg_type  = RTM_DELLINK;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;

    int pos = NLMSG_HDRLEN;

    struct ifinfomsg *ifi = (struct ifinfomsg *)(buf + pos);
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index  = (int)ifindex;
    pos += (int)NLMSG_ALIGN(sizeof(*ifi));

    int ret = nl_transact(ctx, buf, pos);
    return (ret == -ENODEV || ret == -ENOENT) ? 0 : ret;
}

/* ════════════════════════════════════════════════════════════════════════
 * CAKE qdisc helpers
 * ════════════════════════════════════════════════════════════════════════ */

/*
 * Build the TCA_OPTIONS nested attribute block for CAKE with all opts.
 * Called by both the "add" and "change" paths.
 */
static void tc__cake_fill_opts(char *buf, int *pos,
                               uint64_t rate_Bps,
                               const cake_qdisc_opts_t *opts)
{
    int o = nl_nest_start(buf, pos, TCA_OPTIONS);

    nl_put_u64(buf, pos, TCA_CAKE_BASE_RATE64, rate_Bps);

    if (opts) {
        nl_put_u32(buf, pos, TCA_CAKE_DIFFSERV_MODE, opts->diffserv);
        nl_put_u32(buf, pos, TCA_CAKE_ATM,           opts->atm);
        nl_put_u32(buf, pos, TCA_CAKE_FLOW_MODE,     opts->flow_mode);

        /* overhead: omit if INT32_MIN (let CAKE keep its default of 0) */
        if (opts->overhead != INT32_MIN)
            nl_put_s32(buf, pos, TCA_CAKE_OVERHEAD, opts->overhead);

        if (opts->mpu)
            nl_put_u32(buf, pos, TCA_CAKE_MPU, opts->mpu);

        nl_put_u32(buf, pos, TCA_CAKE_NAT,        opts->nat);
        nl_put_u32(buf, pos, TCA_CAKE_WASH,        opts->wash);
        nl_put_u32(buf, pos, TCA_CAKE_INGRESS,     opts->ingress);
        nl_put_u32(buf, pos, TCA_CAKE_ACK_FILTER,  opts->ack_filter);
        nl_put_u32(buf, pos, TCA_CAKE_SPLIT_GSO,   opts->split_gso);

        if (opts->rtt_us)
            nl_put_u32(buf, pos, TCA_CAKE_RTT, opts->rtt_us);
    }

    nl_nest_end(buf, pos, o);
}

/*
 * tc__cake_qdisc_op  –  send one RTM_NEWQDISC for CAKE.
 *
 *   nl_flags = 0                            → qdisc change (modify in-place)
 *   nl_flags = NLM_F_CREATE | NLM_F_EXCL   → qdisc add   (new qdisc)
 *   nl_flags = NLM_F_CREATE                 → qdisc replace
 */
static int tc__cake_qdisc_op(tc_nl_ctx_t *ctx,
                              unsigned int ifindex, uint32_t parent,
                              uint64_t rate_Bps,
                              const cake_qdisc_opts_t *opts,
                              uint16_t nl_flags)
{
    char buf[NL_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    nlh->nlmsg_type  = RTM_NEWQDISC;
    nlh->nlmsg_flags = (uint16_t)(NLM_F_REQUEST | NLM_F_ACK | nl_flags);

    int pos = NLMSG_HDRLEN;

    struct tcmsg *tc = (struct tcmsg *)(buf + pos);
    tc->tcm_family  = AF_UNSPEC;
    tc->tcm_ifindex = (int)ifindex;
    tc->tcm_handle  = 0;
    tc->tcm_parent  = parent;
    pos += (int)NLMSG_ALIGN(sizeof(*tc));

    /* Select qdisc kind: always plain "cake" */
    nl_put_str(buf, &pos, TCA_KIND, "cake");
    tc__cake_fill_opts(buf, &pos, rate_Bps, opts);

    return nl_transact(ctx, buf, pos);
}

/*
 * tc__qdisc_del  –  RTM_DELQDISC for any qdisc type.
 * Silently returns 0 for ENOENT / EINVAL (qdisc not present).
 */
static int tc__qdisc_del(tc_nl_ctx_t *ctx,
                          unsigned int ifindex,
                          uint32_t parent, uint32_t handle)
{
    char buf[NL_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    nlh->nlmsg_type  = RTM_DELQDISC;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;

    int pos = NLMSG_HDRLEN;

    struct tcmsg *tc = (struct tcmsg *)(buf + pos);
    tc->tcm_family  = AF_UNSPEC;
    tc->tcm_ifindex = (int)ifindex;
    tc->tcm_handle  = handle;
    tc->tcm_parent  = parent;
    pos += (int)NLMSG_ALIGN(sizeof(*tc));

    int ret = nl_transact(ctx, buf, pos);
    if (ret == -ENOENT || ret == -EINVAL)
        return 0;
    return ret;
}

/* ════════════════════════════════════════════════════════════════════════
 * Ingress qdisc
 * ════════════════════════════════════════════════════════════════════════ */

/*
 * tc__ingress_add  –  attach the clsact/ingress qdisc to <ifindex>.
 *
 * Equivalent to:  tc qdisc add dev <if> ingress
 */
static int tc__ingress_add(tc_nl_ctx_t *ctx, unsigned int ifindex)
{
    char buf[NL_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    nlh->nlmsg_type  = RTM_NEWQDISC;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;

    int pos = NLMSG_HDRLEN;

    struct tcmsg *tc = (struct tcmsg *)(buf + pos);
    tc->tcm_family  = AF_UNSPEC;
    tc->tcm_ifindex = (int)ifindex;
    tc->tcm_handle  = 0xFFFF0000U;   /* standard ingress handle */
    tc->tcm_parent  = TC_H_INGRESS;
    pos += (int)NLMSG_ALIGN(sizeof(*tc));

    nl_put_str(buf, &pos, TCA_KIND, "ingress");

    int ret = nl_transact(ctx, buf, pos);
    if (ret == -EEXIST)
        return 0;   /* already present is fine */
    return ret;
}

/*
 * tc__ingress_del  –  remove the ingress qdisc from <ifindex>.
 * Silently succeeds if not present.
 */
static int tc__ingress_del(tc_nl_ctx_t *ctx, unsigned int ifindex)
{
    return tc__qdisc_del(ctx, ifindex, TC_H_INGRESS, 0xFFFF0000U);
}

/* ════════════════════════════════════════════════════════════════════════
 * matchall + mirred egress redirect filter
 * ════════════════════════════════════════════════════════════════════════ */

/*
 * tc__mirred_filter_add  –  attach a matchall filter on the ingress qdisc
 * of <src_ifindex> that redirects all traffic to <dst_ifindex>.
 *
 * We use the "matchall" classifier (cls_matchall, kernel 4.12+) rather than
 * "u32 match u32 0 0" because:
 *   • matchall is purpose-built for this exact use case
 *   • it never inspects packet data and has no hash-table overhead
 *   • u32's implicit hash-table creation can silently mis-match on some
 *     kernel configs, leaving the ingress qdisc with no active filter
 *     (which is exactly the symptom: IFB tx_bytes stays at 0)
 *
 * Equivalent tc command:
 *   tc filter add dev <src> parent ffff: protocol all \
 *       matchall action mirred egress redirect dev <dst>
 */
static int tc__mirred_filter_add(tc_nl_ctx_t *ctx,
                                  unsigned int src_ifindex,
                                  unsigned int dst_ifindex)
{
    char buf[NL_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    nlh->nlmsg_type  = RTM_NEWTFILTER;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;

    int pos = NLMSG_HDRLEN;

    struct tcmsg *tc = (struct tcmsg *)(buf + pos);
    tc->tcm_family  = AF_UNSPEC;
    tc->tcm_ifindex = (int)src_ifindex;
    tc->tcm_handle  = 0;
    tc->tcm_parent  = 0xFFFF0000U;   /* ingress qdisc handle ffff: */
    /*
     * tcm_info encodes (prio << 16) | protocol (network byte order).
     * protocol = ETH_P_ALL = 0x0003, must be big-endian in this field.
     */
    tc->tcm_info = TC_H_MAKE(1U << 16, (uint32_t)htons(ETH_P_ALL));
    pos += (int)NLMSG_ALIGN(sizeof(*tc));

    /* ── Classifier: matchall ────────────────────────────────────── */
    nl_put_str(buf, &pos, TCA_KIND, "matchall");

    int opts = nl_nest_start(buf, &pos, TCA_OPTIONS);

    /* ── Action list nested inside TCA_OPTIONS ───────────────────── */
    int act_list = nl_nest_start(buf, &pos, TCA_MATCHALL_ACT);
    int act1     = nl_nest_start(buf, &pos, 1);  /* first action slot */

    nl_put_str(buf, &pos, TCA_ACT_KIND, "mirred");

    int mact_opts = nl_nest_start(buf, &pos, TCA_ACT_OPTIONS);

    /*
     * struct tc_mirred wire layout (kernel UAPI, linux/tc_act/tc_mirred.h):
     *   uint32  index    – 0 = kernel assigns
     *   uint32  capab    – 0
     *   int32   action   – TC_ACT_STOLEN (4): packet is consumed by the action
     *   int32   refcnt   – 0
     *   int32   bindcnt  – 0
     *   int32   eaction  – TCA_EGRESS_REDIR (1)
     *   uint32  ifindex  – destination interface
     */
    struct {
        uint32_t index;
        uint32_t capab;
        int32_t  action;
        int32_t  refcnt;
        int32_t  bindcnt;
        int32_t  eaction;
        uint32_t ifindex;
    } mp;
    memset(&mp, 0, sizeof(mp));
    mp.action  = TC_ACT_STOLEN;
    mp.eaction = TCA_EGRESS_REDIR;
    mp.ifindex = dst_ifindex;

    nl_put_attr(buf, &pos, TCA_MIRRED_PARMS, &mp, sizeof(mp));

    nl_nest_end(buf, &pos, mact_opts);
    nl_nest_end(buf, &pos, act1);
    nl_nest_end(buf, &pos, act_list);
    nl_nest_end(buf, &pos, opts);

    int ret = nl_transact(ctx, buf, pos);
    if (ret == -EEXIST)
        return 0;
    return ret;
}

/* ════════════════════════════════════════════════════════════════════════
 * Public: runtime rate control
 * ════════════════════════════════════════════════════════════════════════ */

int tc_cake_set_bandwidth(tc_nl_ctx_t *ctx,
                          const char  *iface,
                          uint32_t     rate_kbps)
{
    if (!ctx || !iface || !iface[0]) {
        errno = EINVAL;
        return -1;
    }

    unsigned int ifindex = if_nametoindex(iface);
    if (!ifindex) {
        syslog(LOG_WARNING, "tc_netlink: unknown interface '%s'", iface);
        errno = ENODEV;
        return -1;
    }

    /* kbps → bytes/sec: kbps * 1000 / 8 = kbps * 125 */
    uint64_t rate_Bps = (uint64_t)rate_kbps * 125ULL;

    /* Modify existing root CAKE qdisc in-place (no NLM_F_CREATE). */
    int ret = tc__cake_qdisc_op(ctx, ifindex, TC_H_ROOT, rate_Bps, NULL, 0);
    if (ret == 0)
        return 0;

    /*
     * Use cached opts so all CAKE options (diffserv, flow mode,
     * NAT, wash, overhead, etc.) are preserved – not just the rate.
     */
    if (errno == ENOENT) {
        const cake_qdisc_opts_t *saved = tc__lookup_opts(ctx, ifindex);
        ret = tc__cake_qdisc_op(ctx, ifindex, TC_H_ROOT, rate_Bps, saved,
                                NLM_F_CREATE | NLM_F_EXCL);
        if (ret == 0)
            return 0;
    }

    syslog(LOG_WARNING,
           "tc_netlink: failed to set CAKE bandwidth on '%s' to %u kbps: %m",
           iface, rate_kbps);
    return -1;
}

/* ════════════════════════════════════════════════════════════════════════
 * Public: download path lifecycle
 * ════════════════════════════════════════════════════════════════════════ */

int tc_dl_setup(tc_nl_ctx_t            *ctx,
                const char             *wan_if,
                const char             *ifb_if,
                uint32_t                rate_kbps,
                const cake_qdisc_opts_t *opts_dl)
{
    if (!ctx || !wan_if || !wan_if[0] || !ifb_if || !ifb_if[0]) {
        errno = EINVAL;
        return -1;
    }

    int ret;
    uint64_t rate_Bps = (uint64_t)rate_kbps * 125ULL;

    /* ── 1. Create IFB interface ─────────────────────────────── */
    ret = tc__link_create_ifb(ctx, ifb_if);
    if (ret < 0 && errno != EEXIST) {
        syslog(LOG_ERR, "tc_dl_setup: create IFB '%s': %m", ifb_if);
        return -1;
    }

    /* ── 2. Bring IFB up ─────────────────────────────────────── */
    ret = tc__link_set_up(ctx, ifb_if);
    if (ret < 0) {
        syslog(LOG_ERR, "tc_dl_setup: set IFB '%s' up: %m", ifb_if);
        return -1;
    }

    unsigned int ifb_idx = if_nametoindex(ifb_if);
    unsigned int wan_idx = if_nametoindex(wan_if);
    if (!ifb_idx || !wan_idx) {
        syslog(LOG_ERR, "tc_dl_setup: interface lookup failed");
        return -1;
    }

    /* ── 3. CAKE root qdisc on IFB ───────────────────────────── */
    /*
     * Try change first; if absent, add.  This makes setup idempotent
     * if the daemon is restarted without a full teardown.
     */
    ret = tc__cake_qdisc_op(ctx, ifb_idx, TC_H_ROOT, rate_Bps, opts_dl, 0);
    if (ret < 0 && errno == ENOENT)
        ret = tc__cake_qdisc_op(ctx, ifb_idx, TC_H_ROOT, rate_Bps, opts_dl,
                                NLM_F_CREATE | NLM_F_EXCL);
    if (ret < 0 && errno != EEXIST) {
        syslog(LOG_ERR, "tc_dl_setup: CAKE qdisc on '%s': %m", ifb_if);
        return -1;
    }

    /* Cache the DL opts so tc_cake_set_bandwidth fallback can use them */
    if (opts_dl)
        tc__cache_opts(ctx, ifb_idx, opts_dl);

    /* ── 4. Ingress qdisc on WAN ─────────────────────────────── */
    ret = tc__ingress_add(ctx, wan_idx);
    if (ret < 0) {
        syslog(LOG_ERR, "tc_dl_setup: ingress qdisc on '%s': %m", wan_if);
        return -1;
    }

    /* ── 5. u32 match-all + mirred redirect WAN → IFB ───────── */
    ret = tc__mirred_filter_add(ctx, wan_idx, ifb_idx);
    if (ret < 0) {
        syslog(LOG_ERR, "tc_dl_setup: mirred filter '%s'→'%s': %m",
               wan_if, ifb_if);
        return -1;
    }

    syslog(LOG_INFO, "tc_dl_setup: DL path ready (%s→%s @ %u kbps)",
           wan_if, ifb_if, rate_kbps);
    return 0;
}

void tc_dl_teardown(tc_nl_ctx_t *ctx,
                    const char  *wan_if,
                    const char  *ifb_if)
{
    if (!ctx)
        return;

    /* Filters are removed automatically when the ingress qdisc is deleted. */

    if (wan_if && wan_if[0]) {
        unsigned int wan_idx = if_nametoindex(wan_if);
        if (wan_idx)
            tc__ingress_del(ctx, wan_idx);
    }

    if (ifb_if && ifb_if[0]) {
        unsigned int ifb_idx = if_nametoindex(ifb_if);
        if (ifb_idx)
            tc__qdisc_del(ctx, ifb_idx, TC_H_ROOT, 0);

        /* Deleting the IFB link also removes its qdiscs. */
        tc__link_delete(ctx, ifb_if);
    }

    syslog(LOG_INFO, "tc_dl_teardown: DL path removed (%s / %s)",
           wan_if ? wan_if : "?", ifb_if ? ifb_if : "?");
}

/* ════════════════════════════════════════════════════════════════════════
 * Public: upload path lifecycle
 * ════════════════════════════════════════════════════════════════════════ */

int tc_ul_setup(tc_nl_ctx_t            *ctx,
                const char             *wan_if,
                uint32_t                rate_kbps,
                const cake_qdisc_opts_t *opts_ul)
{
    if (!ctx || !wan_if || !wan_if[0]) {
        errno = EINVAL;
        return -1;
    }

    unsigned int wan_idx = if_nametoindex(wan_if);
    if (!wan_idx) {
        syslog(LOG_ERR, "tc_ul_setup: unknown interface '%s'", wan_if);
        errno = ENODEV;
        return -1;
    }

    uint64_t rate_Bps = (uint64_t)rate_kbps * 125ULL;

    /*
     * Always delete any existing root qdisc before creating our CAKE qdisc.
     *
     * Rationale: the "change" path (RTM_NEWQDISC without NLM_F_CREATE) is
     * unreliable for ensuring all CAKE opts are applied when:
     *  (a) The existing qdisc is a different type (pfifo_fast, fq_codel …)
     *      – the kernel returns 0 after a no-op or EINVAL, leaving the
     *        wrong qdisc in place.
     *  (b) The existing qdisc is CAKE but was created by SQM scripts without
     *      overhead/MPU – CAKE's cake_change() may not reset already-zero
     *      fields when the attribute is sent explicitly as 0.
     *  (c) Wireless interfaces (phy/wlan) on OpenWrt often have a
     *      per-driver default qdisc that is NOT replaced by a plain "change".
     *
     * Deleting first and creating fresh guarantees all opts are always applied.
     * The brief moment without a shaper during setup is acceptable since this
     * only runs at daemon start or interface recovery.
     */
    tc__qdisc_del(ctx, wan_idx, TC_H_ROOT, 0);  /* silently ignores ENOENT */

    int ret = tc__cake_qdisc_op(ctx, wan_idx, TC_H_ROOT, rate_Bps, opts_ul,
                                NLM_F_CREATE | NLM_F_EXCL);

    if (ret < 0 && errno == EEXIST) {
        /*
         * Extremely unlikely race (another process created a qdisc between
         * our delete and create).  Fall back to a change-in-place which will
         * at least update the rate.
         */
        syslog(LOG_WARNING,
               "tc_ul_setup: EEXIST race on '%s', applying change in-place",
               wan_if);
        ret = tc__cake_qdisc_op(ctx, wan_idx, TC_H_ROOT, rate_Bps, opts_ul, 0);
    }

    if (ret < 0) {
        syslog(LOG_ERR, "tc_ul_setup: CAKE qdisc on '%s': %m", wan_if);
        return -1;
    }

    /* Cache the UL opts so tc_cake_set_bandwidth fallback can use them */
    if (opts_ul)
        tc__cache_opts(ctx, wan_idx, opts_ul);

    syslog(LOG_INFO, "tc_ul_setup: UL path ready (%s @ %u kbps)",
           wan_if, rate_kbps);
    return 0;
}

void tc_ul_teardown(tc_nl_ctx_t *ctx, const char *wan_if)
{
    if (!ctx || !wan_if || !wan_if[0])
        return;

    unsigned int wan_idx = if_nametoindex(wan_if);
    if (wan_idx)
        tc__qdisc_del(ctx, wan_idx, TC_H_ROOT, 0);

    syslog(LOG_INFO, "tc_ul_teardown: UL path removed (%s)", wan_if);
}

/* ════════════════════════════════════════════════════════════════════════
 * Public: qdisc statistics dump (per-tin stats)
 * ════════════════════════════════════════════════════════════════════════ */

/*
 * sch_cake exposes statistics through the TCA_STATS2 → TCA_STATS_APP
 * netlink attribute nest, produced by its .dump_stats callback:
 *
 *   TCA_STATS2
 *     TCA_STATS_APP
 *       TCA_CAKE_STATS_*            (qdisc-wide: capacity, memory, …)
 *       TCA_CAKE_STATS_TIN_STATS    (nest)
 *         [1..N]                    (one nest per tin)
 *           TCA_CAKE_TIN_STATS_*    (u32, or u64 with PAD)
 *
 * This is the netlink equivalent of `tc -s qdisc show dev <if> cake`.
 * Note: only plain "cake" implements .dump_stats — a cake-mq root qdisc
 * has no per-tin statistics at all.
 */

/* TCA_CAKE_STATS_* attribute values (linux/pkt_sched.h). */
#ifndef TCA_CAKE_STATS_TIN_STATS
#define TCA_CAKE_STATS_PAD              1
#define TCA_CAKE_STATS_CAPACITY_ESTIMATE64 2
#define TCA_CAKE_STATS_MEMORY_LIMIT     3
#define TCA_CAKE_STATS_MEMORY_USED      4
#define TCA_CAKE_STATS_TIN_STATS        10
#define TCA_CAKE_STATS_ACTIVE_QUEUES    17
#endif

/* TCA_CAKE_TIN_STATS_* attribute values (linux/pkt_sched.h). */
#ifndef TCA_CAKE_TIN_STATS_PAD
#define TCA_CAKE_TIN_STATS_PAD                  1
#define TCA_CAKE_TIN_STATS_SENT_PACKETS         2
#define TCA_CAKE_TIN_STATS_SENT_BYTES64         3
#define TCA_CAKE_TIN_STATS_DROPPED_PACKETS      4
#define TCA_CAKE_TIN_STATS_DROPPED_BYTES64      5
#define TCA_CAKE_TIN_STATS_ECN_MARKED_PACKETS   8
#define TCA_CAKE_TIN_STATS_ECN_MARKED_BYTES64   9
#define TCA_CAKE_TIN_STATS_BACKLOG_BYTES        11
#define TCA_CAKE_TIN_STATS_THRESHOLD_RATE64     12
#define TCA_CAKE_TIN_STATS_TARGET_US            13
#define TCA_CAKE_TIN_STATS_INTERVAL_US          14
#define TCA_CAKE_TIN_STATS_WAY_MISSES           16
#define TCA_CAKE_TIN_STATS_WAY_COLLISIONS       17
#define TCA_CAKE_TIN_STATS_PEAK_DELAY_US        18
#define TCA_CAKE_TIN_STATS_AVG_DELAY_US         19
#define TCA_CAKE_TIN_STATS_BASE_DELAY_US        20
#define TCA_CAKE_TIN_STATS_SPARSE_FLOWS         21
#define TCA_CAKE_TIN_STATS_BULK_FLOWS           22
#define TCA_CAKE_TIN_STATS_UNRESPONSIVE_FLOWS   23
#endif

#ifndef TCA_STATS_APP
#define TCA_STATS_APP 4
#endif

/* Largest possible dump reply: 8 tins × ~25 attrs ≈ 3 KiB. */
#define CAKE_DUMP_BUF_SIZE 8192

static uint32_t nla_get_u32_val(const struct nlattr *a)
{
    uint32_t v;
    memcpy(&v, (const char *)a + NLA_HDRLEN, sizeof(v));
    return v;
}

static uint64_t nla_get_u64_val(const struct nlattr *a)
{
    uint64_t v;
    memcpy(&v, (const char *)a + NLA_HDRLEN, sizeof(v));
    return v;
}

/* Decode one TCA_CAKE_TIN_STATS_* attribute list into a tin struct. */
static void tc__parse_tin(const struct nlattr *tin, cake_tin_stats_t *t)
{
    memset(t, 0, sizeof(*t));

    int rem = tin->nla_len - NLA_HDRLEN;
    const struct nlattr *a =
        (const struct nlattr *)((const char *)tin + NLA_HDRLEN);

    while (rem >= (int)sizeof(*a) &&
           a->nla_len >= NLA_HDRLEN && a->nla_len <= rem) {
        switch (nla_stripped_type(a)) {
        case TCA_CAKE_TIN_STATS_THRESHOLD_RATE64:    t->threshold_bps     = nla_get_u64_val(a); break;
        case TCA_CAKE_TIN_STATS_SENT_PACKETS:        t->sent_packets      = nla_get_u32_val(a); break;
        case TCA_CAKE_TIN_STATS_SENT_BYTES64:        t->sent_bytes        = nla_get_u64_val(a); break;
        case TCA_CAKE_TIN_STATS_DROPPED_PACKETS:     t->dropped_packets   = nla_get_u32_val(a); break;
        case TCA_CAKE_TIN_STATS_DROPPED_BYTES64:     t->dropped_bytes     = nla_get_u64_val(a); break;
        case TCA_CAKE_TIN_STATS_ECN_MARKED_PACKETS:  t->ecn_packets       = nla_get_u32_val(a); break;
        case TCA_CAKE_TIN_STATS_ECN_MARKED_BYTES64:  t->ecn_bytes         = nla_get_u64_val(a); break;
        case TCA_CAKE_TIN_STATS_BACKLOG_BYTES:       t->backlog_bytes     = nla_get_u32_val(a); break;
        case TCA_CAKE_TIN_STATS_TARGET_US:           t->target_us         = nla_get_u32_val(a); break;
        case TCA_CAKE_TIN_STATS_INTERVAL_US:         t->interval_us       = nla_get_u32_val(a); break;
        case TCA_CAKE_TIN_STATS_PEAK_DELAY_US:       t->peak_delay_us     = nla_get_u32_val(a); break;
        case TCA_CAKE_TIN_STATS_AVG_DELAY_US:        t->avg_delay_us      = nla_get_u32_val(a); break;
        case TCA_CAKE_TIN_STATS_BASE_DELAY_US:       t->base_delay_us     = nla_get_u32_val(a); break;
        case TCA_CAKE_TIN_STATS_WAY_MISSES:          t->way_misses        = nla_get_u32_val(a); break;
        case TCA_CAKE_TIN_STATS_WAY_COLLISIONS:      t->way_collisions    = nla_get_u32_val(a); break;
        case TCA_CAKE_TIN_STATS_SPARSE_FLOWS:        t->sparse_flows      = nla_get_u32_val(a); break;
        case TCA_CAKE_TIN_STATS_BULK_FLOWS:          t->bulk_flows        = nla_get_u32_val(a); break;
        case TCA_CAKE_TIN_STATS_UNRESPONSIVE_FLOWS:  t->unresp_flows      = nla_get_u32_val(a); break;
        default: break;
        }

        rem -= (int)NLA_ALIGN(a->nla_len);
        a = (const struct nlattr *)((const char *)a + NLA_ALIGN(a->nla_len));
    }
}

/* Decode the TCA_STATS_APP nest (qdisc-wide stats + tin stats nest). */
static void tc__parse_cake_app(const struct nlattr *app, cake_qdisc_stats_t *out)
{
    int rem = app->nla_len - NLA_HDRLEN;
    const struct nlattr *a =
        (const struct nlattr *)((const char *)app + NLA_HDRLEN);

    while (rem >= (int)sizeof(*a) &&
           a->nla_len >= NLA_HDRLEN && a->nla_len <= rem) {
        switch (nla_stripped_type(a)) {
        case TCA_CAKE_STATS_CAPACITY_ESTIMATE64:
            out->capacity_bps = nla_get_u64_val(a);
            break;
        case TCA_CAKE_STATS_MEMORY_LIMIT:
            out->memory_limit = nla_get_u32_val(a);
            break;
        case TCA_CAKE_STATS_MEMORY_USED:
            out->memory_used = nla_get_u32_val(a);
            break;
        case TCA_CAKE_STATS_ACTIVE_QUEUES:
            out->active_queues = nla_get_u32_val(a);
            break;
        case TCA_CAKE_STATS_TIN_STATS: {
            /* Per-tin nests carry nla_type 1..N; kernel emits them
             * in tin display order (bulk … voice). */
            int trem = a->nla_len - NLA_HDRLEN;
            const struct nlattr *ta =
                (const struct nlattr *)((const char *)a + NLA_HDRLEN);

            while (trem >= (int)sizeof(*ta) &&
                   ta->nla_len >= NLA_HDRLEN && ta->nla_len <= trem) {
                unsigned int idx = nla_stripped_type(ta);   /* 1-based */
                if (idx >= 1 && idx <= 8 &&
                    out->tin_cnt < 8) {
                    tc__parse_tin(ta, &out->tins[out->tin_cnt]);
                    out->tin_cnt++;
                }
                trem -= (int)NLA_ALIGN(ta->nla_len);
                ta = (const struct nlattr *)((const char *)ta + NLA_ALIGN(ta->nla_len));
            }
            break;
        }
        default: break;
        }

        rem -= (int)NLA_ALIGN(a->nla_len);
        a = (const struct nlattr *)((const char *)a + NLA_ALIGN(a->nla_len));
    }
}

/*
 * Decode one RTM_NEWQDISC reply message.
 * Only messages for req_ifindex are considered: the kernel ignores
 * tcm_ifindex on GETQDISC dumps and returns every qdisc in the system
 * (like `tc qdisc show` without dev filter), so userspace must filter
 * (like iproute2 does for `tc qdisc show dev <if>`).
 * Returns 1 if it was this interface's plain CAKE qdisc (stats decoded),
 * 0 otherwise.
 */
static int tc__parse_qdisc_msg(const struct nlmsghdr *nlh,
                               unsigned int req_ifindex,
                               cake_qdisc_stats_t *out)
{
    const struct tcmsg *tcm = (const struct tcmsg *)NLMSG_DATA(nlh);
    if ((unsigned int)tcm->tcm_ifindex != req_ifindex)
        return 0;

    char kind[32] = "";
    const struct nlattr *stats2 = NULL;

    int rem = (int)NLMSG_PAYLOAD(nlh, sizeof(struct tcmsg));
    /* Attributes start AFTER the tcmsg header (iproute2: NLMSG_DATA + ALIGN(sizeof tcmsg)). */
    const struct nlattr *a =
        (const struct nlattr *)((const char *)NLMSG_DATA(nlh) +
                                NLMSG_ALIGN(sizeof(struct tcmsg)));

    while (rem >= (int)sizeof(*a) &&
           a->nla_len >= NLA_HDRLEN && a->nla_len <= rem) {
        switch (nla_stripped_type(a)) {
        case TCA_KIND:
            snprintf(kind, sizeof(kind), "%s", (const char *)a + NLA_HDRLEN);
            break;
        case TCA_STATS2:
            stats2 = a;
            break;
        default: break;
        }

        rem -= (int)NLA_ALIGN(a->nla_len);
        a = (const struct nlattr *)((const char *)a + NLA_ALIGN(a->nla_len));
    }

    /* Exact match: plain cake only (no cake_mq / cake-mq stats exist). */
    if (strcmp(kind, "cake") != 0 || !stats2)
        return 0;

    int rem2 = stats2->nla_len - NLA_HDRLEN;
    const struct nlattr *s =
        (const struct nlattr *)((const char *)stats2 + NLA_HDRLEN);

    while (rem2 >= (int)sizeof(*s) &&
           s->nla_len >= NLA_HDRLEN && s->nla_len <= rem2) {
        if (nla_stripped_type(s) == TCA_STATS_APP) {
            snprintf(out->kind, sizeof(out->kind), "%s", kind);
            tc__parse_cake_app(s, out);
            return 1;
        }
        rem2 -= (int)NLA_ALIGN(s->nla_len);
        s = (const struct nlattr *)((const char *)s + NLA_ALIGN(s->nla_len));
    }

    return 0;
}

int tc_cake_get_stats(tc_nl_ctx_t *ctx, const char *iface,
                      cake_qdisc_stats_t *out)
{
    if (!ctx || !iface || !iface[0] || !out) {
        errno = EINVAL;
        return -1;
    }

    unsigned int ifindex = if_nametoindex(iface);
    if (!ifindex) {
        errno = ENODEV;
        return -1;
    }

    /* ── RTM_GETQDISC dump request, filtered by ifindex ──────── */
    char buf[128];
    memset(buf, 0, sizeof(buf));

    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    nlh->nlmsg_type  = RTM_GETQDISC;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;

    int pos = NLMSG_HDRLEN;
    struct tcmsg *tc = (struct tcmsg *)(buf + pos);
    tc->tcm_family  = AF_UNSPEC;
    tc->tcm_ifindex = (int)ifindex;
    pos += (int)NLMSG_ALIGN(sizeof(*tc));

    nlh->nlmsg_len = (uint32_t)pos;
    nlh->nlmsg_seq = ctx->seq++;
    nlh->nlmsg_pid = 0;

    struct sockaddr_nl dst;
    memset(&dst, 0, sizeof(dst));
    dst.nl_family = AF_NETLINK;

    if (sendto(ctx->fd, buf, pos, 0,
               (struct sockaddr *)&dst, sizeof(dst)) < 0)
        return -1;

    /* ── Collect replies until NLMSG_DONE ──────────────────────
     *
     * A dump reply is multipart: one recv() datagram can pack several
     * RTM_NEWQDISC messages plus the terminating NLMSG_DONE, so every
     * message in the buffer must be walked (NLMSG_OK / NLMSG_NEXT).
     * Parsing only the first header per recv() silently drops the rest
     * (e.g. cake arriving after ingress on the same interface).
     */
    memset(out, 0, sizeof(*out));
    int found = 0;
    int done = 0;

    uint8_t rbuf[CAKE_DUMP_BUF_SIZE];
    while (!done) {
        ssize_t n = recv(ctx->fd, rbuf, sizeof(rbuf), 0);
        if (n < (ssize_t)NLMSG_HDRLEN)
            break;

        struct nlmsghdr *h = (struct nlmsghdr *)rbuf;
        int rem = (int)n;

        for (; NLMSG_OK(h, rem); h = NLMSG_NEXT(h, rem)) {
            /* Skip unsolicited notifications (link events etc.) */
            if (h->nlmsg_seq != nlh->nlmsg_seq)
                continue;

            if (h->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(h);
                if (err->error) {
                    errno = -err->error;
                    return -1;
                }
                done = 1;
                break;
            }

            if (h->nlmsg_type == NLMSG_DONE) {
                done = 1;
                break;
            }

            if (h->nlmsg_type == RTM_NEWQDISC)
                found |= tc__parse_qdisc_msg(h, ifindex, out);
        }
    }

    if (!found) {
        errno = ENOENT;
        return -1;
    }

    return 0;
}
