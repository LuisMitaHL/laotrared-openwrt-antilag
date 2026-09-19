# antilag

A lightweight, CPU-efficient C rewrite of [cake-autorate](https://github.com/lynxthecat/cake-autorate) for OpenWrt. Designed for routers where maximizing CPU efficiency is a priority.

## Background

The original [cake-autorate](https://github.com/lynxthecat/cake-autorate) created by [@lynxthecat](https://github.com/lynxthecat) is a highly effective bash script that dynamically adjusts CAKE bandwidth based on real-time One-Way Delay (OWD) measurements.

While the algorithm is excellent at mitigating bufferbloat, running a complex bash script that continuously spawns new processes and subshells can consume a significant amount of CPU on lower-end routers.

**antilag** resolves this by reimplementing the exact same algorithm as a native C binary and OpenWrt procd service. It drastically reduces CPU overhead while maintaining identical adaptive traffic-shaping behavior.

*All credit for the original algorithm, math, and concept goes to [@lynxthecat](https://github.com/lynxthecat) and the contributors of the original repository.*

---

## Features

- **Native C implementation** — no bash scripts, no subshells, minimal CPU footprint
- **Event-driven architecture** — uses `libubox/uloop` to eliminate polling busy-loops
- **Multi-WAN** — one independent daemon instance per UCI section; shape any number of WAN links alongside policy routing (mwan3)
- **Static or adaptive per instance** — run adaptive shaping on one link and a fixed sqm-scripts-style target rate on another, from the same config and web UI
- **Direct CAKE management** — creates and manages IFB + CAKE qdiscs entirely via raw NETLINK_ROUTE, with no dependency on SQM scripts
- **Efficient system I/O** — reads network statistics directly from `/sys/class/net/.../statistics`
- **Asynchronous pinging** — custom ICMP pinger supporting both Echo (type 8) and Timestamp (type 13) modes
- **Per-direction flow isolation** — separate CAKE `flow_mode` settings for DL ingress and UL egress
- **Dynamic reflector health** — automatically monitors and replaces unresponsive ping targets
- **Flash storage safe** — all state data kept in RAM, zero flash writes at runtime
- **LuCI web interface** — fully integrated UI for configuration, service control, and live status

---

## How It Works

The daemon continuously measures latency using ICMP and maintains an asymmetric EWMA baseline per reflector to detect genuine bufferbloat versus normal variance. It classifies network load into four states and adjusts the CAKE shaper rate accordingly:

| State | Condition | Action |
| :--- | :--- | :--- |
| **BUFFERBLOAT** | OWD delta exceeds configured threshold | Reduces shaper rate aggressively |
| **HIGH** | Achieved rate > `high_load_thr × shaper_rate` | Increases shaper rate |
| **LOW** | Achieved rate > `connection_active_thr` | Decays shaper rate toward base rate |
| **IDLE** | Minimal traffic detected | Decays shaper rate toward base rate |

By default the daemon uses `ping_type 0` (ICMP Echo, RTT/2 as OWD proxy). For more accurate per-direction OWD on asymmetric links such as 5G/LTE, `ping_type 1` (ICMP Timestamp) is recommended where reflectors support it.

---

## Installation

### Prerequisites

```sh
apk update
apk add libubox libuci
```

### From a Release

Download the appropriate `.apk` for your architecture from the Releases page, upload it to your router, and install:

```sh
apk add --allow-untrusted antilag_*.apk
```

Enable and start the service:

```sh
/etc/init.d/antilag enable
/etc/init.d/antilag start
```

### From Source (OpenWrt SDK)

```sh
make package/antilag/compile V=s
```

---

## Configuration

The recommended way to configure antilag is through the LuCI web interface at **Services → Antilag**. Clicking Save & Apply will automatically reload the daemon.

Alternatively, edit the config file directly over SSH:

```sh
vi /etc/config/antilag
```

The minimum required options are the interface names and rate limits. Ensure `dl_if` and `ul_if` match your router's actual interfaces — for typical setups, download traffic arrives on an IFB interface and upload on the physical WAN interface.

```
config antilag 'wan'
    option enabled                  '1'
    option dl_if                    'ifb4wan'
    option ul_if                    'wan'
    option base_dl_shaper_rate_kbps '50000'
    option base_ul_shaper_rate_kbps '20000'
    option max_dl_shaper_rate_kbps  '100000'
    option max_ul_shaper_rate_kbps  '35000'
```

### CAKE Options

CAKE qdisc options such as `overhead`, `mpu`, `rtt`, `memlimit`, and `wash` are configured separately for DL and UL. Flow isolation mode can also be set independently per direction — for example `dual-dsthost` on ingress and `dual-srchost` on egress, which is the recommended setup for most home routers.

### Multi-WAN (mwan3)

The daemon supports any number of WAN links: **each UCI section is an independent instance** that shapes one WAN pair. Instances can be added and removed on the Services → Antilag page (or directly in `/etc/config/antilag`):

```sh
config antilag 'wan1'
    option enabled                  '1'
    option dl_if                    'ifb-wan1'
    option ul_if                    'wan1'
    option ping_bind_if             'wan1'
    option base_dl_shaper_rate_kbps '50000'
    option base_ul_shaper_rate_kbps '20000'

config antilag 'wan2'
    option enabled                  '1'
    option dl_if                    'ifb-wan2'
    option ul_if                    'wan2'
    option ping_bind_if             'wan2'
    option base_dl_shaper_rate_kbps '20000'
    option base_ul_shaper_rate_kbps '10000'
```

Requirements and notes:

- **Unique interface names** — each instance needs its own `dl_if` (IFB, e.g. `ifb-wan1`, created automatically) and `ul_if`. Never point two sections at the same `ul_if`.
- **`ping_bind_if`** — binds the ICMP measurement socket to that interface (`SO_BINDTODEVICE`). Required for multi-WAN: without it, policy routing (mwan3) may send reflector pings out an arbitrary WAN, corrupting the per-WAN OWD measurement. Typically the same value as `ul_if`. Leave empty on single-WAN setups to follow the routing table.
- **mwan3 interplay** — antilag only touches qdiscs; it does not conflict with mwan3's nftables marking or routing rules. mwan3 handles per-flow routing and load balancing, antilag handles per-WAN bufferbloat control. mwan3's own `track` pings are unaffected.
- Each instance keeps its own reflector list and status file (`/var/run/antilag-<section>.json`), and shows up as a separate status block on the LuCI Overview page.

### Static Instances (sqm-scripts replacement)

Every instance has a **Mode**:

| Mode | Behaviour |
| :--- | :--- |
| `dynamic` (default) | Adaptive: pings reflectors, measures OWD and adjusts the CAKE rates continuously. Runs as a procd daemon. |
| `static` | Fixed target download/upload rate, no reflectors and no live changes. The sqm-scripts model. |

A static instance applies CAKE once and then stays out of the way — there is
no daemon, no pinging and no rate monitor. This is what you want when:

- you only need a fixed shaper (a guest network, a backup link, …);
- another tool owns the adaptive logic on that link;
- you want to replace **sqm-scripts** without running both at once.

```
config antilag 'guest'
    option enabled '1'
    option mode 'static'
    option dl_if 'ifb-guest'
    option ul_if 'br-guest'
    option base_dl_shaper_rate_kbps '50000'
    option base_ul_shaper_rate_kbps '20000'
```

In static mode the `base_*_shaper_rate_kbps` values are the fixed targets and
all reflector/OWD options are ignored. The CAKE qdisc options (`cake_overhead`,
`cake_mpu`, `cake_nat`, `cake_diffserv`, `cake_flow_mode`, …) still apply.

Because a static instance has no daemon, its qdiscs are re-created by the
interface hotplug hook `/etc/hotplug.d/iface/25-antilag` — exactly like
sqm-scripts — whenever the `ul_if` interface comes up (PPPoE reconnect, DHCP
renew, modem re-registration). The hook also tears the qdiscs down on
`ifdown`. A state file in `/var/run/antilag-<section>.state` records the
interfaces so `stop` can clean up even after the UCI section is deleted.

> **Note:** do not point an antilag instance and an sqm-scripts `queue`
> section at the same interface — both create CAKE qdiscs and they will fight.
> Use a static antilag instance *instead of* SQM for that link.

### 5G / LTE Notes

On cellular links the UL scheduling latency is inherently higher and more variable than DL, even at idle, due to the base station grant request cycle. If your idle UL OWD delta appears elevated (5–10ms) with no load, this is normal radio behaviour and not a misconfiguration. Consider:

- Setting `ping_type 1` (ICMP Timestamp) for true per-direction OWD rather than RTT/2
- Slightly raising `alpha_baseline_increase` (e.g. `0.005`) to let the baseline track the natural idle jitter floor of your link

---

## Live Status

The LuCI Overview page displays a live status widget that polls every 3 seconds, with one status block per configured antilag instance. When a daemon is running it shows:

| Field | Description |
| :--- | :--- |
| Status | Current autorate state (Running / Idle / Stall) |
| DL / UL Shaped | Current CAKE shaper rate |
| DL / UL Actual | Measured achieved throughput |
| DL / UL Load | Load classification (High / Low / Idle / Bufferbloat) |
| OWD DL / UL Δ | One-way delay delta above baseline — turns red above +10ms |
| Uptime | Time since daemon started |

Below the status row, the widget shows **live per-tin CAKE statistics** (the netlink equivalent of `tc -s qdisc show`) for the download (IFB) and upload (WAN) qdiscs, one table per direction:

| Field | Description |
| :--- | :--- |
| Tin | Tin number in CAKE's display order (bulk → voice) |
| Threshold | The tin's configured share of the shaper bandwidth |
| Sent | Packets and bytes forwarded through the tin |
| Dropped | Packets and bytes dropped by the AQM (red when non-zero) |
| ECN Marks | Packets ECN-marked instead of dropped |
| Backlog | Bytes currently queued in the tin |
| Avg Delay | Average queuing delay in the tin |

The header line also shows CAKE's capacity estimate and qdisc memory usage. Statistics are read from the kernel via an `RTM_GETQDISC` dump; they are only available for plain `cake` qdiscs and are omitted while the qdisc is down.

The same live status is also shown on the Services → Antilag page.

The daemon writes `/var/run/antilag-<section>.json` every ~200ms, querying the kernel qdisc statistics on each tick. The file is removed on clean shutdown so the widget immediately reflects stopped state.

---

## Verification and Logging

Confirm the service is running under procd:

```sh
ubus call service list '{"name":"antilag"}'
```

Watch the daemon adjust bandwidth in real time:

```sh
logread -f -e antilag
```

Inspect the live status JSON directly (one file per instance):

```sh
cat /var/run/antilag-*.json
```

---

## Credits

**antilag** is a rebrand of **darkmoon** for Laotrared. It carries no functional
changes over the project it forks — all credit for the design and code goes to
the original projects:

| Project | Author | Repository | Package names |
| :--- | :--- | :--- | :--- |
| [cake-autorate](https://github.com/lynxthecat/cake-autorate) | [@lynxthecat](https://github.com/lynxthecat) | https://github.com/lynxthecat/cake-autorate | `cake-autorate` (bash service) — original OWD algorithm, math & concept |
| [openwrt-package-darkmoon](https://github.com/kamikaonashi/openwrt-package-darkmoon) | [kamikaonashi](https://github.com/kamikaonashi) | https://github.com/kamikaonashi/openwrt-package-darkmoon | `darkmoon`, `luci-app-darkmoon` — C rewrite & OpenWrt/LuCI integration this fork is based on |
| [timestamp-reflectors](https://github.com/tievolu/timestamp-reflectors) | [tievolu](https://github.com/tievolu) | https://github.com/tievolu/timestamp-reflectors | Reflector list recommended for ICMP Timestamp mode |
| [antilag](https://github.com/LuisMitaHL/laotrared-openwrt-antilag) | Laotrared | https://github.com/LuisMitaHL/laotrared-openwrt-antilag | `antilag`, `luci-app-antilag` — this rebrand |

## License

This project is released under the MIT License.

The original cake-autorate project is licensed under its own terms. Please see the upstream repository for details.

---

## Screenshots

<img width="3343" height="1318" alt="Screenshot From 2026-02-26 17-26-41" src="https://github.com/user-attachments/assets/2dde3238-859f-4f90-86a1-b57384f253cf" />
<img width="3343" height="1318" alt="Screenshot From 2026-02-26 17-26-46" src="https://github.com/user-attachments/assets/d9149822-3a21-4f3c-ab15-c09b0a83507b" />
<img width="3343" height="1318" alt="Screenshot From 2026-02-26 17-26-54" src="https://github.com/user-attachments/assets/132e7d98-eaa3-4a2b-aaf8-70e37d1d5bab" />
<img width="3343" height="1318" alt="Screenshot From 2026-02-26 17-26-59" src="https://github.com/user-attachments/assets/626c132d-e15a-48b2-850f-38aadf523412" />
<img width="3343" height="1318" alt="Screenshot From 2026-02-26 17-27-04" src="https://github.com/user-attachments/assets/59adcf63-c6e1-4805-975c-619120fd4bf1" />
