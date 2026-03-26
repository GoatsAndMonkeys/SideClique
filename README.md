# TinyBBS SideClique

**A peer-to-peer decentralized Mesh BBS for families and friend groups.**

Every node is a BBS. Every member syncs with every other member. Messages, status, locations, and commands flow between your family or friend group — even when you're never all online at the same time.

No internet. No central server. No single point of failure.

---

## Features

### Implemented

| Feature | Status | Description |
|---|---|---|
| **Multi-Clique** | ✅ | Each encrypted channel = a clique. Up to 8 simultaneous cliques |
| **Invisible Back Channel** | ✅ | Protocol uses port 256 (invisible to Meshtastic app). Chat stays clean |
| **Check-In Board** | ✅ | See everyone's status, battery, city, last seen — grouped by clique |
| **Persistent DMs** | ✅ | Messages queue and retry until delivered. 7-day expiry |
| **Status Updates** | ✅ | One-tap: OK, HELP, TRAVELING, HOME, AWAY |
| **SOS (self)** | ✅ | 30-second GPS broadcasts, alerts all members |
| **Wordle** | ✅ | Daily word game. Upload wordle.bin for full 12,972-word dictionary |
| **Chess by Mesh** | ✅ | Full chess with alpha-beta AI, board rendering, game save/load |
| **City Name Lookup** | ✅ | Locations show as city names (optional, requires geo_us.bin on flash) |
| **E-ink/OLED Display** | ✅ | Shows clique count, member count, pending DMs, SOS status |
| **Encryption Required** | ✅ | Refuses to activate on unencrypted channels |
| **Auto-Discovery** | ✅ | Members auto-added when heard on encrypted channel |
| **Canary Timeout** | ✅ | Members marked "??" after 24 hours with no beacon |

### Planned (Not Yet Implemented)

| Feature | Phase | Description |
|---|---|---|
| Remote SOS | 2 | Parent triggers SOS on child's node — child cannot cancel |
| LOCATE | 2 | Remote GPS tracking — broadcast position every 60s |
| Remote PING | 2 | Instant status/battery/location check on any member |
| Remote ALERT | 2 | Buzz/flash a member's node with a message |
| Gossip Sync | 2 | Vector clock delta sync — messages propagate transitively |
| Parental Controls | 2 | ADMIN role can override MEMBER nodes |
| DM Relay | 2 | Any member can relay DMs to offline targets |
| Shared Bulletin Board | 3 | Group announcements synced across all members |
| Shared Lists (CRDT) | 3 | Grocery, supplies, tasks — conflict-free edits |
| Rally Points | 3 | Pre-set meeting locations |
| Dead Drops | 3 | GPS-tagged messages that appear when nearby |
| Family Wordle Leaderboard | 3 | Scores synced within clique |
| Flash Persistence | 3 | Save state to external flash (survives reboot) |
| Geo-fencing | 4 | Alert when member enters/leaves an area |
| Battery Prediction | 4 | Estimate time remaining from drain rate |
| Movement Detection | 4 | "Alex is heading toward Rally Point 2" |

---

## How Multi-Clique Works

Each encrypted Meshtastic channel automatically becomes a clique. Protocol traffic is invisible to the Meshtastic app:

```
Channel 0 "Family" (encrypted, PSK=abc)
  Port 1:   "Hey Mom!" .............. visible in Meshtastic app
  Port 256: [BEACON][SYNC][DM] ...... SideClique only, invisible

Channel 1 "Work" (encrypted, PSK=xyz)
  Port 1:   "Meeting at 3" .......... visible in Meshtastic app
  Port 256: [BEACON][SYNC][DM] ...... SideClique only, invisible

Channel 2 (no encryption)
  SideClique ignores this channel
```

No setup commands needed — the encrypted channel IS the clique.

---

## Menu

DM the SideClique node to interact:

```
SideClique [2 cliques 5 members]
[C]heck-in board
[D]M send
[S]tatus update
[P]ing member
[F]ind member
[!]SOS
[W]ordle
[K]Chess by Mesh
[X]Exit
```

### Check-In Board

```
=== Family ===
Dad      OK    2m   85%  Portland
Mom      OK   15m   72%  Portland
Alex     TRVL  3h   45%  Seattle
Sam      ??    2d    --
=== Work ===
Boss     AWAY  1h   60%  Seattle
```

---

## Module Sizes (T-Echo Build)

### SideClique Modules

| Module | Size | What it contains |
|---|---|---|
| SideClique.cpp | 8.6 KB | Core: multi-clique, check-in, DMs, status, SOS, menus, Wordle |
| BBSChess.cpp | 8.5 KB | Chess: move gen, alpha-beta AI, board rendering, persistence |
| BBSExtFlash.cpp | 1.6 KB | QSPI external flash driver + LittleFS |
| BBSWordle.h | ~1 KB | Wordle word list + game logic (header-only) |
| BBSGeoLookup.h | ~0.5 KB | City lookup from embedded/external database |
| BBSGeoDB.h | ~11 KB | 500 US cities (embedded fallback) |
| **Total SideClique** | **~32 KB** | |

### Active Meshtastic Modules

| Module | Size | What it does |
|---|---|---|
| Admin | 16.6 KB | Device configuration |
| TraceRoute | 10.7 KB | Mesh route tracing |
| Position/GPS | 6.8 KB | GPS position handling |
| KeyVerification | 4.4 KB | PKI key verification |
| ExternalNotification | 4.3 KB | Buzzer/LED alerts |
| NeighborInfo | 4.0 KB | Neighbor node sharing |
| Serial | 2.7 KB | Serial passthrough |
| NodeInfo | 2.3 KB | Core node info |

### Excluded Meshtastic Modules (saved ~37 KB)

| Module | Savings | What it does |
|---|---|---|
| CannedMessages | 17.9 KB | Pre-written button messages (no keyboard on most nodes) |
| ATAK | 3.8 KB | Military TAK mapping integration |
| MQTT | ~3 KB | Internet bridge (no WiFi on nRF52) |
| StoreForward | ~3 KB | Store-and-forward relay (uses too much RAM) |
| RemoteHardware | 2.2 KB | Remote GPIO pin control |
| Power Telemetry | ~2 KB | INA219/INA260 power monitors |
| Waypoint | 2.3 KB | GPS waypoint sharing |
| DetectionSensor | 1.6 KB | PIR motion sensor |
| RangeTest | ~1.5 KB | Automated range testing |
| Audio | ~1 KB | Audio modem (SX1280 only) |
| Dropzone | ~0.5 KB | Skydiving altimeter |

### Flash Summary (T-Echo)

| | Size |
|---|---|
| Firmware used | 732 KB (89.8%) |
| Free | 83 KB (10.2%) |
| External flash | 2 MB (for geo, wordle, future data) |

---

## Supported Hardware

| Board | Ext Flash | Screen | Notes |
|---|---|---|---|
| LilyGO T-Echo | 2 MB | E-ink | Primary target, tested |
| RAK4631 WisBlock | 1 MB | OLED/E-ink | Unified display support |
| Heltec Mesh Node T114 | 2 MB | TFT | Small color screen |
| Nano G2 Ultra | 2 MB | — | Compact |
| Seeed Wio Tracker | 2 MB | Screen | Outdoor-ready |
| ProMicro DIY | None | — | Basic, no geo/wordle |
| Tracker T1000-E | None | None | Card-sized tracker |

---

## Quick Start

### 1. Flash firmware

Download from [Releases](../../releases):

**T-Echo**: Double-press reset → copy UF2 to TECHOBOOT volume
**RAK4631**: Double-press reset → copy UF2 to RAK4631 volume
**Other nRF52 boards**: Enter bootloader → copy UF2 to mounted volume

### 2. Set up encrypted channel

Set a PSK on your Meshtastic primary channel. Share it with family/friends. Everyone on the encrypted channel is automatically in the clique.

> **Important**: SideClique refuses to activate without encryption.

### 3. Use it

DM the node — you'll see the SideClique menu. Members auto-discover as they're heard on the mesh.

---

## Optional: City Names & Family Wordle

Upload data files to external flash for enhanced features:

```bash
pip install pyserial
python3 scripts/serial_upload.py upload data/geo_us.bin /clique/kb/geo_us.bin
python3 scripts/serial_upload.py upload data/wordle.bin /clique/kb/wordle.bin
```

---

## Build From Source

```bash
git clone https://github.com/GoatsAndMonkeys/SideClique.git
cd SideClique
git clone --recurse-submodules https://github.com/meshtastic/firmware.git
python3 -m venv .venv312
source .venv312/bin/activate
pip install platformio
./scripts/integrate.sh
cd firmware && pio run -e t-echo
```

---

## Encryption & Security

- **AES-256** encryption via Meshtastic channel PSK
- **Auto-clique** — encrypted channel = your clique. No setup commands needed
- **Refuses to run unencrypted** — shows error if no PSK set
- **Invisible protocol** — sync/beacon traffic on port 256, invisible to Meshtastic app
- **Anti-replay** — sequence numbers + timestamps reject old/duplicate messages
- **Parental controls** (planned) — ADMIN role can override MEMBER nodes

---

## Architecture

```
/clique/
├── 0/                    Channel 0 clique
│   ├── members/          Status, position, battery per member
│   ├── inbox/            Received DMs
│   ├── outbox/           Pending DMs (retry until delivered)
│   └── vector.bin        Sync state
├── 1/                    Channel 1 clique
│   └── ...
└── kb/                   Shared knowledge base (optional)
    ├── geo_us.bin        16,726 US cities
    └── wordle.bin        12,972 five-letter words
```

---

## License

MIT

---

*Built on [Meshtastic](https://meshtastic.org/) firmware. Part of the [TinyBBS](https://github.com/GoatsAndMonkeys/TinyBBS) ecosystem.*
