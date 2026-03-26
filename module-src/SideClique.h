#pragma once
// TinyBBS SideClique — a peer-to-peer decentralized Mesh BBS
//
// Every node is a BBS. Members sync via gossip protocol over encrypted channel.
// Persistent DMs, status board, SOS/LOCATE, shared lists.

#include "mesh/SinglePortModule.h"
#include "concurrency/OSThread.h"
#include "BBSChess.h"
#include "BBSWordle.h"

// Clique message types
#define SC_MSG_BEACON    0x01
#define SC_MSG_SYNC_REQ  0x02
#define SC_MSG_SYNC_DATA 0x03
#define SC_MSG_DM        0x10
#define SC_MSG_DM_ACK    0x11
#define SC_MSG_DM_READ   0x12
#define SC_MSG_STATUS    0x20
#define SC_MSG_BULLETIN  0x30
#define SC_MSG_LIST_OP   0x40
#define SC_MSG_LOCATE    0x50
#define SC_MSG_PING      0x51
#define SC_MSG_ALERT     0x52
#define SC_MSG_SOS       0xFF

// Member roles
#define SC_ROLE_MEMBER   0
#define SC_ROLE_ADMIN    1  // parent/guardian — can override child nodes

// Member statuses
#define SC_STATUS_UNKNOWN  0  // ?? — no beacon in >24h
#define SC_STATUS_OK       1
#define SC_STATUS_HELP     2
#define SC_STATUS_TRAVELING 3
#define SC_STATUS_HOME     4
#define SC_STATUS_AWAY     5
#define SC_STATUS_SOS      6

// Limits
#define SC_MAX_CLIQUES     8       // max channels = max cliques
#define SC_MAX_MEMBERS     16      // per clique
#define SC_MAX_DM_QUEUE    32      // total across all cliques
#define SC_MAX_BULLETINS   20
#define SC_MAX_RALLY       8
#define SC_BEACON_INTERVAL_S  900  // 15 minutes
#define SC_LOCATE_INTERVAL_S  60   // 1 minute during locate
#define SC_SOS_INTERVAL_S     30   // 30 seconds during SOS
#define SC_CANARY_TIMEOUT_S   86400 // 24 hours

static constexpr uint8_t SC_MAX_SESSIONS = 4;

// ─── Data structures ────────────────────────────────────────────────────

struct SCMember {
    uint32_t nodeNum;
    char     name[12];       // short name
    uint8_t  role;           // ADMIN or MEMBER
    uint8_t  status;         // OK, HELP, TRAVELING, etc.
    int32_t  latitude;       // position_i format
    int32_t  longitude;
    int32_t  altitude;
    uint8_t  battery;        // 0-100
    uint32_t lastSeen;       // timestamp of last beacon
    uint32_t syncSeq;        // highest message seq from this member
    char     location[24];   // city name from geo lookup
    bool     locateActive;   // remote GPS tracking active
    bool     sosActive;      // SOS mode active
};

struct SCPendingDM {
    uint32_t id;
    uint32_t to;             // destination node
    uint32_t created;        // timestamp
    uint32_t expires;        // TTL
    uint32_t lastRetry;      // last retry timestamp
    uint8_t  retries;
    uint8_t  status;         // 0=queued, 1=sent, 2=delivered, 3=read
    char     text[160];
};

struct SCBulletin {
    uint32_t seq;
    uint32_t from;
    uint32_t timestamp;
    char     fromName[12];
    char     text[160];
};

// A clique = one encrypted Meshtastic channel
// Protocol traffic uses port 256 (PRIVATE_APP) — invisible to Meshtastic app
// Human chat uses port 1 (TEXT_MESSAGE_APP) — visible in app
struct SCClique {
    uint8_t  channelIndex;     // 0-7
    bool     active;           // has PSK = active clique
    char     name[16];         // channel name
    SCMember members[SC_MAX_MEMBERS];
    uint8_t  memberCount;
    uint32_t mySeq;            // our message sequence counter for this clique
};

// Menu states
enum SCMenuState : uint8_t {
    SC_STATE_IDLE = 0,
    SC_STATE_MAIN,
    SC_STATE_CHECKIN,
    SC_STATE_DM_LIST,
    SC_STATE_DM_SEND_TO,
    SC_STATE_DM_SEND_BODY,
    SC_STATE_BOARD,
    SC_STATE_LISTS,
    SC_STATE_RALLY,
    SC_STATE_SETUP,
    SC_STATE_CHESS,
    SC_STATE_WORDLE,
};

struct SCSession {
    uint32_t    nodeNum;
    SCMenuState state;
    uint32_t    lastActivity;
    uint32_t    dmSendTo;    // target for DM being composed
    uint32_t    chessGameId; // active chess game (0=none)
    // Wordle
    char        wordleTarget[6];
    uint8_t     wordleGuesses;
    uint32_t    wordleDay;
};

// ─── Module ─────────────────────────────────────────────────────────────

class SideClique : public SinglePortModule, private concurrency::OSThread {
  private:
    SCClique    cliques_[SC_MAX_CLIQUES]; // one per encrypted channel
    uint8_t     activeCliques_ = 0;
    SCSession   sessions_[SC_MAX_SESSIONS];
    uint32_t    lastBeacon_ = 0;
    bool        initialized_ = false;
    bool        locateMode_ = false;
    bool        sosMode_ = false;

    // Session management
    SCSession *getOrCreateSession(uint32_t nodeNum);
    void expireSessions(uint32_t now);

    // Clique management
    void scanCliques();
    SCClique *getClique(uint8_t channelIndex);

    // Member management
    SCMember *findMember(uint32_t nodeNum);
    SCMember *findMemberInClique(SCClique &clique, uint32_t nodeNum);
    SCMember *addMember(uint32_t nodeNum, const char *name, uint8_t role);
    SCMember *addMemberToClique(SCClique &clique, uint32_t nodeNum, const char *name, uint8_t role);
    void updateMemberLocation(SCMember *m);

    // Sync protocol
    void sendBeacon();
    void handleBeacon(const meshtastic_MeshPacket &mp, const uint8_t *data, size_t len);
    void handleSyncReq(const meshtastic_MeshPacket &mp, const uint8_t *data, size_t len);
    void handleSyncData(const meshtastic_MeshPacket &mp, const uint8_t *data, size_t len);

    // Message handling
    void handleCliqueDM(const meshtastic_MeshPacket &mp, const uint8_t *data, size_t len);
    void handleDMAck(const meshtastic_MeshPacket &mp, const uint8_t *data, size_t len);
    void handleStatusUpdate(const meshtastic_MeshPacket &mp, const uint8_t *data, size_t len);
    void handleBulletin(const meshtastic_MeshPacket &mp, const uint8_t *data, size_t len);

    // Remote commands
    void handleLocate(const meshtastic_MeshPacket &mp, const uint8_t *data, size_t len);
    void handlePing(const meshtastic_MeshPacket &mp, const uint8_t *data, size_t len);
    void handleAlert(const meshtastic_MeshPacket &mp, const uint8_t *data, size_t len);
    void handleSOS(const meshtastic_MeshPacket &mp, const uint8_t *data, size_t len);

    // DM queue
    void queueDM(uint32_t to, const char *text);
    void retryPendingDMs(uint32_t now);
    void sendQueuedDM(SCPendingDM &dm);

    // User interface (DM menu)
    void sendMainMenu(const meshtastic_MeshPacket &req);
    void sendCheckinBoard(const meshtastic_MeshPacket &req);
    void sendInbox(const meshtastic_MeshPacket &req);
    void sendBulletinBoard(const meshtastic_MeshPacket &req);

    ProcessMessage handleUserDM(const meshtastic_MeshPacket &mp, const char *text);
    ProcessMessage dispatchState(const meshtastic_MeshPacket &mp, SCSession &session, const char *text);
    ProcessMessage handleStateMain(const meshtastic_MeshPacket &mp, SCSession &session, const char *text);
    ProcessMessage handleStateCheckin(const meshtastic_MeshPacket &mp, SCSession &session, const char *text);
    ProcessMessage handleStateDMList(const meshtastic_MeshPacket &mp, SCSession &session, const char *text);
    ProcessMessage handleStateDMSendTo(const meshtastic_MeshPacket &mp, SCSession &session, const char *text);
    ProcessMessage handleStateDMSendBody(const meshtastic_MeshPacket &mp, SCSession &session, const char *text);
    ProcessMessage handleStateWordle(const meshtastic_MeshPacket &mp, SCSession &session, const char *text);
    ProcessMessage handleStateChess(const meshtastic_MeshPacket &mp, SCSession &session, const char *text);
    void doWordleStart(const meshtastic_MeshPacket &req, SCSession &session);
    void sendChessStatus(const meshtastic_MeshPacket &req, uint32_t gameId);
    uint32_t wordleDay();

    // Helpers
    bool sendReply(const meshtastic_MeshPacket &req, const char *text);
    bool sendCliquePacket(uint8_t msgType, const uint8_t *data, size_t len, uint32_t dest = 0);
    bool sendCliquePacketOnChannel(uint8_t msgType, const uint8_t *data, size_t len, uint8_t channel, uint32_t dest = 0);
    const char *statusStr(uint8_t status);
    const char *getNodeShortName(uint32_t nodeNum);

    // Storage
    void loadState();
    void saveState();
    void loadDMQueue();
    void saveDMQueue();

    // OSThread
    virtual int32_t runOnce() override;

  public:
    SideClique();
    virtual ~SideClique();
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override;

#if HAS_SCREEN
    virtual bool wantUIFrame() override { return true; }
    virtual void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) override;
#endif
};

extern SideClique *sideClique;
