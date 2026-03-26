// TinyBBS SideClique — a peer-to-peer decentralized Mesh BBS
// Phase 1: Check-in board, persistent DMs, beacons, basic sync

#include "SideClique.h"
#include "Channels.h"
#include "FSCommon.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "RTC.h"
#include "modules/RoutingModule.h"
#include <Arduino.h>
#include <cctype>
#include <cstdio>
#include <cstring>

#ifdef NRF52_SERIES
#include "BBSExtFlash.h"
#endif
#ifndef BBS_LITE
#include "BBSGeoLookup.h"
#endif

SideClique *sideClique;

static const char *STATUS_NAMES[] = {"??", "OK", "HELP", "TRVL", "HOME", "AWAY", "SOS"};

// ─── Constructor / Destructor ─────────────────────────────────────────

SideClique::SideClique()
    : SinglePortModule("sclq", meshtastic_PortNum_TEXT_MESSAGE_APP),
      concurrency::OSThread("sideclique") {
    memset(members_, 0, sizeof(members_));
    memset(sessions_, 0, sizeof(sessions_));
}

SideClique::~SideClique() {}

// ─── Status string ────────────────────────────────────────────────────

const char *SideClique::statusStr(uint8_t status) {
    if (status <= SC_STATUS_SOS) return STATUS_NAMES[status];
    return "??";
}

// ─── Node name helper ─────────────────────────────────────────────────

const char *SideClique::getNodeShortName(uint32_t nodeNum) {
    const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(nodeNum);
    if (node && node->has_user && node->user.short_name[0])
        return node->user.short_name;
    return nullptr;
}

// ─── Member management ────────────────────────────────────────────────

SCMember *SideClique::findMember(uint32_t nodeNum) {
    for (uint8_t i = 0; i < memberCount_; i++) {
        if (members_[i].nodeNum == nodeNum) return &members_[i];
    }
    return nullptr;
}

SCMember *SideClique::addMember(uint32_t nodeNum, const char *name, uint8_t role) {
    SCMember *m = findMember(nodeNum);
    if (m) return m;
    if (memberCount_ >= SC_MAX_MEMBERS) return nullptr;

    m = &members_[memberCount_++];
    memset(m, 0, sizeof(SCMember));
    m->nodeNum = nodeNum;
    m->role = role;
    m->status = SC_STATUS_UNKNOWN;
    if (name) {
        strncpy(m->name, name, sizeof(m->name) - 1);
    } else {
        snprintf(m->name, sizeof(m->name), "!%04x", nodeNum & 0xFFFF);
    }
    return m;
}

void SideClique::updateMemberLocation(SCMember *m) {
    if (!m) return;
    m->location[0] = '\0';

#ifndef BBS_LITE
    if (m->latitude != 0 || m->longitude != 0) {
        float lat = m->latitude / 1e7f;
        float lon = m->longitude / 1e7f;
#ifdef NRF52_SERIES
        geoLookupFromExtFlash(lat, lon, m->location, sizeof(m->location));
#else
        geoLookupNearest(lat, lon, m->location, sizeof(m->location));
#endif
    }
#endif
}

// ─── Session management ───────────────────────────────────────────────

SCSession *SideClique::getOrCreateSession(uint32_t nodeNum) {
    for (int i = 0; i < SC_MAX_SESSIONS; i++) {
        if (sessions_[i].nodeNum == nodeNum) {
            sessions_[i].lastActivity = getTime();
            return &sessions_[i];
        }
    }
    // Find empty or oldest
    int oldest = 0;
    for (int i = 1; i < SC_MAX_SESSIONS; i++) {
        if (sessions_[i].lastActivity < sessions_[oldest].lastActivity) oldest = i;
    }
    SCSession *s = &sessions_[oldest];
    memset(s, 0, sizeof(SCSession));
    s->nodeNum = nodeNum;
    s->state = SC_STATE_IDLE;
    s->lastActivity = getTime();
    return s;
}

void SideClique::expireSessions(uint32_t now) {
    for (int i = 0; i < SC_MAX_SESSIONS; i++) {
        if (sessions_[i].nodeNum != 0 && (now - sessions_[i].lastActivity) > 600) {
            sessions_[i].nodeNum = 0;
            sessions_[i].state = SC_STATE_IDLE;
        }
    }
}

// ─── Reply helper ─────────────────────────────────────────────────────

bool SideClique::sendReply(const meshtastic_MeshPacket &req, const char *text) {
    meshtastic_MeshPacket *reply = allocDataPacket();
    if (!reply) return false;
    reply->to = req.from;
    reply->channel = req.channel;
    reply->want_ack = false;
    reply->decoded.want_response = false;
    size_t len = strlen(text);
    if (len > 200) len = 200;
    reply->decoded.payload.size = len;
    memcpy(reply->decoded.payload.bytes, text, len);
    service->sendToMesh(reply);
    return true;
}

// ─── Beacon ───────────────────────────────────────────────────────────

void SideClique::sendBeacon() {
    // Update our own member entry
    SCMember *me = findMember(nodeDB->getNodeNum());
    if (!me) {
        me = addMember(nodeDB->getNodeNum(), getNodeShortName(nodeDB->getNodeNum()), SC_ROLE_ADMIN);
    }
    if (me) {
        me->status = sosMode_ ? SC_STATUS_SOS : SC_STATUS_OK;
        me->lastSeen = getTime();
        me->battery = 0; // TODO: read from powerStatus
        me->syncSeq = mySeq_;

        const meshtastic_NodeInfoLite *myNode = nodeDB->getMeshNode(nodeDB->getNodeNum());
        if (myNode && myNode->has_position) {
            me->latitude = myNode->position.latitude_i;
            me->longitude = myNode->position.longitude_i;
            me->altitude = myNode->position.altitude;
        }
        updateMemberLocation(me);
    }

    // Build beacon packet: type(1) + status(1) + battery(1) + lat(4) + lon(4) + alt(4) + seq(4) = 19 bytes
    uint8_t buf[20];
    buf[0] = SC_MSG_BEACON;
    buf[1] = me ? me->status : SC_STATUS_OK;
    buf[2] = me ? me->battery : 0;
    if (me) {
        memcpy(buf + 3, &me->latitude, 4);
        memcpy(buf + 7, &me->longitude, 4);
        memcpy(buf + 11, &me->altitude, 4);
        memcpy(buf + 15, &mySeq_, 4);
    }

    sendCliquePacket(SC_MSG_BEACON, buf + 1, 18);
    lastBeacon_ = getTime();
}

// ─── Send clique packet on encrypted channel ─────────────────────────

bool SideClique::sendCliquePacket(uint8_t msgType, const uint8_t *data, size_t len, uint32_t dest) {
    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p) return false;

    p->to = dest ? dest : NODENUM_BROADCAST;
    p->channel = channels.getPrimaryIndex(); // TODO: use dedicated clique channel
    p->want_ack = false;
    p->decoded.want_response = false;

    // Pack: msgType(1) + data
    p->decoded.payload.bytes[0] = msgType;
    if (len > 0 && data) {
        memcpy(p->decoded.payload.bytes + 1, data, len > 199 ? 199 : len);
    }
    p->decoded.payload.size = 1 + (len > 199 ? 199 : len);

    service->sendToMesh(p);
    return true;
}

// ─── Handle received packets ──────────────────────────────────────────

bool SideClique::wantPacket(const meshtastic_MeshPacket *p) {
    return SinglePortModule::wantPacket(p);
}

ProcessMessage SideClique::handleReceived(const meshtastic_MeshPacket &mp) {
    if (!mp.decoded.payload.size) return ProcessMessage::CONTINUE;

    // Refuse to operate if primary channel is not encrypted
    if (!initialized_) {
        meshtastic_Channel &primary = channels.getByIndex(channels.getPrimaryIndex());
        bool encrypted = (primary.settings.psk.size > 0);
        if (!encrypted) {
            // Only warn once via DM
            bool isDM = (mp.to == nodeDB->getNodeNum()) && !isBroadcast(mp.to);
            if (isDM) {
                sendReply(mp,
                    "SideClique ERROR:\n"
                    "Primary channel is NOT encrypted.\n"
                    "Set a PSK on your primary channel\n"
                    "to protect your clique's data.");
            }
            return ProcessMessage::STOP;
        }
        initialized_ = true;
    }

    char buf[260] = {0};
    size_t len = mp.decoded.payload.size;
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    memcpy(buf, mp.decoded.payload.bytes, len);
    buf[len] = '\0';

    bool isDM = (mp.to == nodeDB->getNodeNum()) && !isBroadcast(mp.to);

    if (isDM) {
        // User interaction via DM
        return handleUserDM(mp, buf);
    }

    // Broadcast: check for clique protocol messages
    // For now, auto-add any node we hear as a member
    if (mp.from != nodeDB->getNodeNum()) {
        SCMember *m = findMember(mp.from);
        if (!m) {
            m = addMember(mp.from, getNodeShortName(mp.from), SC_ROLE_MEMBER);
        }
        if (m) {
            m->lastSeen = getTime();
            // Update position from nodeDB
            const meshtastic_NodeInfoLite *sender = nodeDB->getMeshNode(mp.from);
            if (sender && sender->has_position) {
                m->latitude = sender->position.latitude_i;
                m->longitude = sender->position.longitude_i;
                m->altitude = sender->position.altitude;
                updateMemberLocation(m);
            }
        }
    }

    return ProcessMessage::CONTINUE;
}

// ─── User DM interface ────────────────────────────────────────────────

ProcessMessage SideClique::handleUserDM(const meshtastic_MeshPacket &mp, const char *text) {
    if (isBroadcast(mp.to)) return ProcessMessage::CONTINUE;

    SCSession *session = getOrCreateSession(mp.from);
    if (!session) return ProcessMessage::CONTINUE;

    return dispatchState(mp, *session, text);
}

ProcessMessage SideClique::dispatchState(const meshtastic_MeshPacket &mp, SCSession &session, const char *text) {
    switch (session.state) {
        case SC_STATE_IDLE:
            session.state = SC_STATE_MAIN;
            return handleStateMain(mp, session, text);
        case SC_STATE_MAIN:
            return handleStateMain(mp, session, text);
        case SC_STATE_CHECKIN:
            return handleStateCheckin(mp, session, text);
        case SC_STATE_DM_SEND_TO:
            return handleStateDMSendTo(mp, session, text);
        case SC_STATE_DM_SEND_BODY:
            return handleStateDMSendBody(mp, session, text);
        default:
            session.state = SC_STATE_MAIN;
            sendMainMenu(mp);
            return ProcessMessage::STOP;
    }
}

// ─── Menus ────────────────────────────────────────────────────────────

void SideClique::sendMainMenu(const meshtastic_MeshPacket &req) {
    char menu[200];
    snprintf(menu, sizeof(menu),
             "SideClique [%u members]\n"
             "[C]heck-in board\n"
             "[D]M send\n"
             "[S]tatus update\n"
             "[P]ing member\n"
             "[F]ind member\n"
             "[!]SOS\n"
             "[K]Chess by Mesh\n"
             "[X]Exit",
             memberCount_);
    sendReply(req, menu);
}

void SideClique::sendCheckinBoard(const meshtastic_MeshPacket &req) {
    char board[512] = "=== SideClique ===\n";
    uint32_t now = getTime();

    for (uint8_t i = 0; i < memberCount_; i++) {
        SCMember &m = members_[i];
        char line[80];
        uint32_t ago = (now > m.lastSeen) ? (now - m.lastSeen) : 0;

        // Check canary
        if (ago > SC_CANARY_TIMEOUT_S && m.status != SC_STATUS_SOS) {
            m.status = SC_STATUS_UNKNOWN;
        }

        const char *agoStr;
        char agoBuf[12];
        if (ago < 60) { snprintf(agoBuf, sizeof(agoBuf), "%us", ago); agoStr = agoBuf; }
        else if (ago < 3600) { snprintf(agoBuf, sizeof(agoBuf), "%um", ago / 60); agoStr = agoBuf; }
        else if (ago < 86400) { snprintf(agoBuf, sizeof(agoBuf), "%uh", ago / 3600); agoStr = agoBuf; }
        else { snprintf(agoBuf, sizeof(agoBuf), "%ud", ago / 86400); agoStr = agoBuf; }

        if (m.location[0])
            snprintf(line, sizeof(line), "%-8s %-4s %4s %3u%% %s\n",
                     m.name, statusStr(m.status), agoStr, m.battery, m.location);
        else
            snprintf(line, sizeof(line), "%-8s %-4s %4s %3u%%\n",
                     m.name, statusStr(m.status), agoStr, m.battery);

        strncat(board, line, sizeof(board) - strlen(board) - 1);
    }

    if (memberCount_ == 0) {
        strncat(board, "(no members yet)\n", sizeof(board) - strlen(board) - 1);
    }
    strncat(board, "[X]Back", sizeof(board) - strlen(board) - 1);
    sendReply(req, board);
}

// ─── State handlers ───────────────────────────────────────────────────

ProcessMessage SideClique::handleStateMain(const meshtastic_MeshPacket &mp, SCSession &session, const char *text) {
    if (!text || text[0] == '\0') { sendMainMenu(mp); return ProcessMessage::STOP; }
    char cmd = tolower((unsigned char)text[0]);

    switch (cmd) {
        case 'c':
            session.state = SC_STATE_CHECKIN;
            sendCheckinBoard(mp);
            break;
        case 'd':
            session.state = SC_STATE_DM_SEND_TO;
            sendReply(mp, "Send DM to whom?\nEnter name or node ID:");
            break;
        case 's': {
            // Quick status update
            sendReply(mp,
                      "Set status:\n"
                      "1.OK 2.HELP\n"
                      "3.TRAVELING 4.HOME\n"
                      "5.AWAY");
            // Handle in next message
            break;
        }
        case '1': case '2': case '3': case '4': case '5': {
            uint8_t newStatus = (cmd - '0');
            SCMember *me = findMember(nodeDB->getNodeNum());
            if (!me) me = addMember(nodeDB->getNodeNum(), getNodeShortName(nodeDB->getNodeNum()), SC_ROLE_ADMIN);
            if (me && newStatus >= SC_STATUS_OK && newStatus <= SC_STATUS_AWAY) {
                me->status = newStatus;
                char reply[60];
                snprintf(reply, sizeof(reply), "Status: %s", statusStr(newStatus));
                sendReply(mp, reply);
                sendBeacon(); // broadcast updated status
            }
            break;
        }
        case 'p': {
            // Ping — show all members with quick status
            sendCheckinBoard(mp);
            break;
        }
        case 'f': {
            sendReply(mp, "Find member — enter name:\n(LOCATE will broadcast their GPS every 60s)");
            // TODO: implement LOCATE state
            break;
        }
        case 'k': {
            // Chess — TODO: refactor BBSChess to be standalone
            sendReply(mp, "Chess coming soon!");
            break;
        }
        case '!': {
            // SOS
            sosMode_ = true;
            SCMember *me = findMember(nodeDB->getNodeNum());
            if (me) me->status = SC_STATUS_SOS;
            sendBeacon();
            sendReply(mp, "SOS ACTIVATED\nBroadcasting position every 30s\nAll members alerted");
            break;
        }
        case 'x':
            session.state = SC_STATE_IDLE;
            sendReply(mp, "SideClique — stay safe!");
            break;
        default:
            sendMainMenu(mp);
            break;
    }
    return ProcessMessage::STOP;
}

ProcessMessage SideClique::handleStateCheckin(const meshtastic_MeshPacket &mp, SCSession &session, const char *text) {
    if (!text || text[0] == '\0' || tolower((unsigned char)text[0]) == 'x') {
        session.state = SC_STATE_MAIN;
        sendMainMenu(mp);
    } else {
        sendCheckinBoard(mp);
    }
    return ProcessMessage::STOP;
}

ProcessMessage SideClique::handleStateDMSendTo(const meshtastic_MeshPacket &mp, SCSession &session, const char *text) {
    if (!text || text[0] == '\0' || tolower((unsigned char)text[0]) == 'x') {
        session.state = SC_STATE_MAIN;
        sendMainMenu(mp);
        return ProcessMessage::STOP;
    }

    // Find member by name or node ID
    uint32_t targetNode = 0;
    for (uint8_t i = 0; i < memberCount_; i++) {
        if (strncasecmp(members_[i].name, text, strlen(text)) == 0) {
            targetNode = members_[i].nodeNum;
            break;
        }
    }
    if (!targetNode) {
        // Try parsing as hex node ID
        targetNode = strtoul(text, nullptr, 16);
    }

    if (!targetNode || !findMember(targetNode)) {
        sendReply(mp, "Member not found. Try again or [X]:");
        return ProcessMessage::STOP;
    }

    session.dmSendTo = targetNode;
    session.state = SC_STATE_DM_SEND_BODY;
    SCMember *m = findMember(targetNode);
    char prompt[60];
    snprintf(prompt, sizeof(prompt), "Message to %s:", m->name);
    sendReply(mp, prompt);
    return ProcessMessage::STOP;
}

ProcessMessage SideClique::handleStateDMSendBody(const meshtastic_MeshPacket &mp, SCSession &session, const char *text) {
    if (!text || text[0] == '\0') {
        session.state = SC_STATE_MAIN;
        sendMainMenu(mp);
        return ProcessMessage::STOP;
    }

    // Queue the DM
    queueDM(session.dmSendTo, text);

    SCMember *m = findMember(session.dmSendTo);
    char reply[80];
    snprintf(reply, sizeof(reply), "DM queued for %s\n%s",
             m ? m->name : "???",
             m && m->lastSeen > 0 ? "Will deliver when seen." : "Member not yet seen on mesh.");
    sendReply(mp, reply);

    session.state = SC_STATE_MAIN;
    return ProcessMessage::STOP;
}

// ─── DM Queue ─────────────────────────────────────────────────────────

// Simple in-memory queue for now — Phase 2 will persist to flash
static SCPendingDM dmQueue_[SC_MAX_DM_QUEUE];
static uint8_t dmQueueCount_ = 0;

void SideClique::queueDM(uint32_t to, const char *text) {
    if (dmQueueCount_ >= SC_MAX_DM_QUEUE) {
        // Drop oldest
        memmove(&dmQueue_[0], &dmQueue_[1], sizeof(SCPendingDM) * (SC_MAX_DM_QUEUE - 1));
        dmQueueCount_--;
    }
    SCPendingDM &dm = dmQueue_[dmQueueCount_++];
    memset(&dm, 0, sizeof(dm));
    dm.id = ++mySeq_;
    dm.to = to;
    dm.created = getTime();
    dm.expires = getTime() + 7 * 86400; // 7 day expiry
    dm.status = 0; // queued
    strncpy(dm.text, text, sizeof(dm.text) - 1);
}

void SideClique::sendQueuedDM(SCPendingDM &dm) {
    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p) return;
    p->to = dm.to;
    p->channel = channels.getPrimaryIndex();
    p->want_ack = true;
    p->decoded.want_response = false;
    size_t len = strlen(dm.text);
    if (len > 200) len = 200;
    p->decoded.payload.size = len;
    memcpy(p->decoded.payload.bytes, dm.text, len);
    service->sendToMesh(p);
    dm.status = 1; // sent
    dm.lastRetry = getTime();
    dm.retries++;
}

void SideClique::retryPendingDMs(uint32_t now) {
    for (uint8_t i = 0; i < dmQueueCount_; i++) {
        SCPendingDM &dm = dmQueue_[i];
        if (dm.status >= 2) continue; // already delivered
        if (now > dm.expires) {
            // Expired — remove
            memmove(&dmQueue_[i], &dmQueue_[i + 1], sizeof(SCPendingDM) * (dmQueueCount_ - i - 1));
            dmQueueCount_--;
            i--;
            continue;
        }

        // Retry schedule: 5min for first hour, 30min for first day, 2hr after
        uint32_t elapsed = now - dm.created;
        uint32_t retryInterval;
        if (elapsed < 3600) retryInterval = 300;       // 5 min
        else if (elapsed < 86400) retryInterval = 1800; // 30 min
        else retryInterval = 7200;                       // 2 hours

        if (now - dm.lastRetry >= retryInterval) {
            // Check if target is recently seen
            SCMember *target = findMember(dm.to);
            if (target && (now - target->lastSeen) < 300) {
                // Target was seen in last 5 min — send now
                sendQueuedDM(dm);
            }
        }
    }
}

// ─── runOnce (periodic tasks) ─────────────────────────────────────────

int32_t SideClique::runOnce() {
    uint32_t t = getTime();
    if (t < 1577836800UL) return 60000; // wait for time sync

    // Send beacon periodically
    uint32_t beaconInterval = sosMode_ ? SC_SOS_INTERVAL_S :
                              locateMode_ ? SC_LOCATE_INTERVAL_S :
                              SC_BEACON_INTERVAL_S;
    if (t - lastBeacon_ >= beaconInterval) {
        sendBeacon();
    }

    // Retry pending DMs
    retryPendingDMs(t);

    // Expire old sessions
    expireSessions(t);

    // Check canary for all members
    for (uint8_t i = 0; i < memberCount_; i++) {
        SCMember &m = members_[i];
        if (m.nodeNum == nodeDB->getNodeNum()) continue;
        if (m.status != SC_STATUS_SOS && m.status != SC_STATUS_UNKNOWN) {
            if (t - m.lastSeen > SC_CANARY_TIMEOUT_S) {
                m.status = SC_STATUS_UNKNOWN;
                // TODO: alert admin members
            }
        }
    }

    return sosMode_ ? 10000 : 30000; // poll faster during SOS
}

// ─── Storage (placeholder — Phase 2 will use external flash) ──────────

void SideClique::loadState() {}
void SideClique::saveState() {}
void SideClique::loadDMQueue() {}
void SideClique::saveDMQueue() {}

// ─── UI Frame ─────────────────────────────────────────────────────────

#if HAS_SCREEN
#include "graphics/ScreenFonts.h"
void SideClique::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) {
    display->setFont(FONT_SMALL);
    display->setColor(OLEDDISPLAY_COLOR::BLACK);
    display->drawString(x, y, "SClq");
    display->setColor(OLEDDISPLAY_COLOR::WHITE);

    char line[32];
    snprintf(line, sizeof(line), "%u members", memberCount_);
    display->drawString(x, y + FONT_HEIGHT_SMALL, line);

    uint8_t pending = 0;
    for (uint8_t i = 0; i < dmQueueCount_; i++)
        if (dmQueue_[i].status < 2) pending++;

    if (pending > 0) {
        snprintf(line, sizeof(line), "%u DMs pending", pending);
        display->drawString(x, y + FONT_HEIGHT_SMALL * 2, line);
    }

    if (sosMode_) {
        display->drawString(x, y + FONT_HEIGHT_SMALL * 3, "!! SOS ACTIVE !!");
    }
}
#endif
