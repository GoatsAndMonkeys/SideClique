# TinyBBS SideClique

**A peer-to-peer decentralized Mesh BBS for families and friend groups.**

Every node is a BBS. Every member syncs with every other member. Messages, status, locations, and commands flow between your family or friend group — even when you're never all online at the same time.

No internet. No central server. No single point of failure.

---

## Features

### Communication
- **Persistent DMs** — messages queue and retry until delivered, even days later
- **Delivery receipts** — Sent → Delivered → Read
- **Shared bulletin board** — group announcements that sync to all members
- **Shared lists** — grocery, supplies, tasks — editable by anyone, syncs automatically

### Safety
- **Check-in board** — see everyone's status, battery, location, and last seen time
- **SOS emergency** — trigger from your node OR remotely on a family member's node
- **Find My Family (LOCATE)** — remotely activate GPS tracking on any member
- **Remote alert** — buzz/flash a family member's node with a message
- **Wellness check** — auto-alert when a member goes quiet too long
- **Parental override** — parents can trigger SOS/LOCATE that children cannot cancel

### Coordination
- **Rally points** — pre-set meeting locations shared across the group
- **Dead drops** — GPS-tagged messages that appear when someone is nearby
- **Remote ping** — instant battery/status/location check on any member

### Optional
- **Family Wordle** — private daily word game with clique-only leaderboard (requires wordle.bin on external flash)
- **City names** — locations show as city names instead of coordinates (requires geo_us.bin on external flash)

---

## How It Works

Members share an encrypted channel (PSK). Nodes sync using a gossip protocol with vector clocks:

1. Two nodes come in range → exchange sync vectors
2. Each side identifies what the other is missing
3. Delta sync — only missing messages are transmitted
4. Messages propagate transitively: Dad → Mom → Alex (even if Dad and Alex never meet)

```
 +-----------+        +-----------+        +-----------+
 |    Dad    |<-sync->|    Mom    |<-sync->|   Alex    |
 |  Portland |        |  Portland |        |  Seattle  |
 |  OK   85% |        |  OK   72% |        |  TRVL 45% |
 +-----------+        +-----------+        +-----------+
      Dad's message to Alex reaches him through Mom
```

---

## Supported Hardware

Any nRF52840 Meshtastic device:

| Board | External Flash | Notes |
|---|---|---|
| LilyGO T-Echo | 2 MB | E-ink display, GPS, tested |
| RAK4631 WisBlock | 1 MB | Modular, OLED/E-ink |
| Heltec Mesh Node T114 | 2 MB | Small TFT display |
| Nano G2 Ultra | 2 MB | Compact |
| Seeed Wio Tracker | 2 MB | Outdoor-ready |
| ProMicro DIY | None | Basic, no geo/wordle |
| Tracker T1000-E | None | Card-sized tracker |

External flash is optional — used for city name lookups and family Wordle dictionary.

---

## Quick Start

### 1. Flash firmware

Download from [Releases](../../releases) and flash via UF2:
- Double-press reset → copy UF2 to TECHOBOOT

### 2. Create a clique

DM the node:
```
!clique create MyFamily
```
It generates a PSK. Share it with family members.

### 3. Join a clique

On each family member's node:
```
!clique join <PSK>
```

### 4. Use it

```
SideClique: MyFamily
[C]heck-in board
[D]M send
[I]nbox
[L]ists
[R]ally points
[S]OS
[F]ind member
[P]ing member
[X]Exit
```

---

## Encryption & Security

- **AES-256** encryption via Meshtastic PSK on a dedicated private channel
- **Anti-replay** — sequence numbers + timestamps reject old/duplicate messages
- **No metadata leaks** — all data stays on clique members' nodes
- **Parental controls** — ADMIN role can remotely command MEMBER nodes
- **Consent** — LOCATE/SOS only work on nodes running SideClique with matching PSK

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

## Optional: City Names & Family Wordle

Upload data files to external flash for enhanced features:

```bash
# Generate data
python3 scripts/gen_geo_packed.py      # city database
python3 scripts/gen_wordle_packed.py   # wordle dictionary

# Upload via serial
python3 scripts/serial_upload.py upload data/geo_us.bin /clique/kb/geo_us.bin
python3 scripts/serial_upload.py upload data/wordle.bin /clique/kb/wordle.bin
```

---

## Architecture

```
/clique/
├── config.bin          Clique settings, member list, roles
├── vector.bin          Sync state (vector clock)
├── members/            Status, position, battery per member
├── inbox/              Received DMs
├── outbox/             Pending DMs (retry until delivered)
├── board/              Shared bulletins
├── lists/              Shared lists (CRDT)
├── rally/              Rally point locations
├── drops/              GPS-tagged dead drops
├── locate/             Location breadcrumbs
└── kb/                 Optional: geo_us.bin, wordle.bin
```

---

## License

MIT

---

*Built on [Meshtastic](https://meshtastic.org/) firmware. Inspired by [TinyBBS](https://github.com/GoatsAndMonkeys/TinyBBS).*
