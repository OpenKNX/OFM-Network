# TODO — ESP32 custom IDF (AutoIP)

Status: **prepared, not active.** The firmware side is done and guarded; what is missing is an ESP-IDF
built with AutoIP enabled. Targets: OAM-IP-Interface (IPIF) and OAM-IP-Router (IPRO).

`OPENKNX_NETWORK_AUTOIP` is set only in `[flags_rp]` of `OAM-IP-Interface/platformio.custom.ini`; its
`[flags_esp32]` does not get it, and OAM-IP-Router does not set it in any env.

---

## Why a custom IDF is needed at all

AutoIP (RFC 3927, link-local `169.254.0.0/16`) is the third address method next to static and DHCP. On
RP2040 it already works — arduino-pico builds lwIP **from source**, so `-D LWIP_AUTOIP=1` in
`build_flags` reaches it. On ESP32 it cannot work, and no firmware change can fix that (line numbers from
framework-arduinoespressif32-libs 5.4.0, the package of arduino-esp32 3.2.1):

```
esp32s3/sdkconfig:2572    # CONFIG_LWIP_AUTOIP is not set
esp32/sdkconfig:1938      # CONFIG_LWIP_AUTOIP is not set
esp32s3/lib/liblwip.a     autoip.c.obj present — but with ZERO symbols
ESP32 firmware.elf        no autoip_* symbols at all
```

`autoip.c` is wholly inside `#if LWIP_AUTOIP`, so with the option off the object file is empty. The
IDF ships lwIP prebuilt; a `-D` on the compiler command line never reaches an already-built library.

Verify after a rebuild:

```bash
xtensa-esp32-elf-nm .pio/build/<env>/firmware.elf | grep autoip
# expected: autoip_start / autoip_network_changed_link_up / autoip_accept_packet ...
```

---

## What to enable

One switch turns on both AutoIP and the DHCP cooperation — the IDF's `lwipopts.h` derives the rest:

```c
#ifdef CONFIG_LWIP_AUTOIP
#define LWIP_AUTOIP                     1
#define LWIP_DHCP_AUTOIP_COOP           1   /* implied, no separate option */
#define LWIP_DHCP_AUTOIP_COOP_TRIES     CONFIG_LWIP_AUTOIP_TRIES
#define LWIP_AUTOIP_MAX_CONFLICTS       CONFIG_LWIP_AUTOIP_MAX_CONFLICTS
#define LWIP_AUTOIP_RATE_LIMIT_INTERVAL CONFIG_LWIP_AUTOIP_RATE_LIMIT_INTERVAL
#endif
```

`menuconfig` → *Component config* → *LWIP*:

| Option | Set to | Note |
|---|---|---|
| `CONFIG_LWIP_AUTOIP` | `y` | default `n`; depends on `LWIP_IPV4` |
| `CONFIG_LWIP_AUTOIP_TRIES` | `4` | IDF default is **2**. 4 is what RP2040 runs: `LWIP_DHCP_AUTOIP_COOP_TRIES=4` in `[flags_rp]` of `OAM-IP-Interface/platformio.custom.ini` (DHCP gets about 30 s before the link-local fallback) |
| `CONFIG_LWIP_AUTOIP_MAX_CONFLICTS` | `9` | IDF default, keep |
| `CONFIG_LWIP_AUTOIP_RATE_LIMIT_INTERVAL` | `20` | IDF default, keep |

Then add `-D OPENKNX_NETWORK_AUTOIP` to the ESP envs (the RP2040 side is `[flags_rp]` in
`platformio.custom.ini`, the ESP side `[flags_esp32]`). Do **not** add the `-D LWIP_*` flags there — on
ESP32 they are meaningless and only create the illusion of a setting.

### Watch out: this changes DHCP conflict detection

`CONFIG_LWIP_DHCP_DOES_ARP_CHECK` is declared `depends on !LWIP_AUTOIP`. Both shipped configs currently
have it enabled (`esp32s3/sdkconfig:2552`). Enabling AutoIP therefore forces the choice to one of:

- `CONFIG_LWIP_DHCP_DOES_ACD_CHECK` — RFC 5227 conflict detection; the IDF help says it "takes several seconds"
  before DHCP binds. Affects **every boot**, not just the AutoIP case.
- `CONFIG_LWIP_DHCP_DOES_NOT_CHECK_OFFERED_IP` — binds immediately, no conflict detection.

Pick deliberately and measure the boot-to-bound time afterwards; this is the one change here that
touches the normal DHCP path of every ESP32 product.

---

## What does NOT need changing

Checked against the shipped configs — these are already set as needed, do not touch them:

| Concern | State | Why it is fine |
|---|---|---|
| Multicast / IGMP (KNXnet/IP routing + discovery) | `LWIP_IGMP 1` hardcoded in the IDF `lwipopts.h`, `igmp_joingroup` present in `liblwip.a` | not a Kconfig option, always on |
| Ethernet W5500 (REG2 ESP) | `CONFIG_ETH_USE_SPI_ETHERNET=y`, `CONFIG_ETH_SPI_ETHERNET_W5500=y` | already set |
| Ethernet native EMAC (REG1 LAN_TP_BASE) | `CONFIG_ETH_USE_ESP32_EMAC=y` | already set (ESP32 classic only; the S3 has no EMAC) |
| Socket count | `CONFIG_LWIP_MAX_SOCKETS=16` | enough for KNX + webserver + MQTT; raise only if the webserver connection cap is raised |
| Multicast socket binding | `CONFIG_LWIP_SO_REUSE=y`, `CONFIG_LWIP_SO_REUSE_RXTOALL=y` | required by the KNX multicast socket, already set |
| WiFi | `CONFIG_ESP_WIFI_ENABLED=y` | **keep enabled.** IPIF/IPRO are LAN products, but the same IDF serves every OpenKNX ESP product and OFM-Network supports `KNX_IP_WIFI`. Trimming it saves flash only if per-product IDFs are built — not worth the fragmentation |
| IPv6 | on | unrelated to AutoIP; leave as is to keep the delta reviewable |

---

## Per product

Nothing product-specific is required — both need exactly the same switches. The difference is only the
PHY, and both PHYs are already configured:

| | IPIF (OAM-IP-Interface) | IPRO (OAM-IP-Router) |
|---|---|---|
| Mask | `0x07B0` + tunnelling | `0x091A` |
| Multicast use | discovery (SEARCH_REQUEST) | discovery **and** routing |
| IGMP needed | yes — already on | yes — already on |
| AutoIP switches | as above | as above |
| ESP32 envs to flag afterwards | `release_REG2_PICO_ESP_ETH_DD` (S3 + W5500), `release_REG1_LAN_TP_BASE` (ESP32 + native LAN8720) | the corresponding router envs |

> Careful with the board names: `release_REG1_ETH` is **RP2040** + W5500, not ESP32 — AutoIP already
> works there. The only ESP32 envs in this product are `REG1_LAN_TP_BASE` and `REG2_PICO_ESP_ETH_DD`.

---

## Firmware side — already done, nothing to write later

- `Module::checkIpAssignmentMethod()` reports `PID_CURRENT_IP_ASSIGNMENT_METHOD` (8 = AutoIP, 4 = DHCP)
  and is driven by the **address**, not by the link edge — AutoIP takes over after the
  `LWIP_DHCP_AUTOIP_COOP_TRIES` DHCP tries (about 30 s with the IP-Interface flag LWIP_DHCP_AUTOIP_COOP_TRIES=4), with the link up the
  whole time. It is platform independent and only gated by
  `OPENKNX_NETWORK_AUTOIP`, so it starts working on ESP32 the moment the flag is set.
- `Module::setLwipLinkState()` is RP2040-only **on purpose**, and this is about the driver, not the chip:
  the arduino-pico W5500 driver never reports the PHY link to lwIP, so the edges have to be fed in by
  hand. On ESP32 `esp_netif_action_connected/disconnected()` already call `netif_set_link_up/down()`
  (`esp_netif_lwip.c:1824` / `:1874` in ESP-IDF 5.5.2), driven by the ETH events that `esp_eth` posts for **any** PHY —
  SPI-attached W5500 and native EMAC alike. Adding the RP2040 call there would be a duplicate.
- `Module.cpp` carries a compile-time guard: setting `OPENKNX_NETWORK_AUTOIP` on ESP32 against an IDF
  without `CONFIG_LWIP_AUTOIP` fails the build instead of shipping a firmware that advertises a method
  the stack cannot perform. **That `#error` names this file by path — do not move or delete it without
  changing `src/OpenKNX/Network/Module.cpp` with it.**

### Timing that applies to this path

- **The AutoIP stage window is 45 s** (`ETH_STAGE_TIMEOUT_MS` in `src/driver/EthLinkManager.h`, under
  `#ifdef OPENKNX_NETWORK_AUTOIP`). AutoIP hands out an address only after its probe and announce
  sequence, so any ESP32 timing has to be read against this window.
- **The carrier read is debounced downwards only, and only on RP2040 + W5500** (`Module::linkCarrier()`); on
  ESP32 it is taken as read. A read that says linked is taken at once, only a not-linked read has to be
  confirmed. The fallback ladder acts on carrier edges, and AutoIP depends on that path to start.

## Acceptance test (same as the RP2040 one)

1. Device on a network with DHCP → address from the server, method reports DHCP.
2. Move the cable to a laptop with no DHCP server. The device runs its REBOOTING attempts, then
   `CONFIG_LWIP_AUTOIP_TRIES` DISCOVERs with the doubling lwIP backoff, then the AutoIP probe and
   announce. Measure the time to link-local against the RP2040 with 4 tries (about 30 s) and against
   the 45 s stage window above.
3. Console `net` shows a `169.254.x.x` address, the log line reads `IP-Address: 169.254.x.x (AutoIP)`.
4. Plug back into the DHCP network → DHCP takes over again, method returns to DHCP.
