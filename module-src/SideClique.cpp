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
    : SinglePortModule("sclq", meshtastic_PortNum_PRIVATE_APP),
      concurrency::OSThread("sideclique") {
    memset(cliques_, 0, sizeof(cliques_));
    memset(sessions_, 0, sizeof(sessions_));
}

// Accept both PRIVATE_APP (protocol) and TEXT_MESSAGE_APP (user DMs)
bool SideClique::wantPacket(const meshtastic_MeshPacket *p) {
    if (p->decoded.portnum == meshtastic_PortNum_PRIVATE_APP) return true;
    if (p->decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP) return true;
    return false;
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

// ─── Clique management ────────────────────────────────────────────────

// Scan channels and activate cliques for encrypted ones
void SideClique::scanCliques() {
    activeCliques_ = 0;
    for (uint8_t i = 0; i < SC_MAX_CLIQUES; i++) {
        meshtastic_Channel &ch = channels.getByIndex(i);
        bool hasKey = (ch.settings.psk.size > 0);
        bool wasActive = cliques_[i].active;
        cliques_[i].channelIndex = i;
        cliques_[i].active = hasKey;
        if (hasKey) {
            activeCliques_++;
            if (!wasActive) {
                // Newly activated clique
                strncpy(cliques_[i].name, ch.settings.name, sizeof(cliques_[i].name) - 1);
                if (cliques_[i].name[0] == '\0')
                    snprintf(cliques_[i].name, sizeof(cliques_[i].name), "Ch%u", i);
            }
        }
    }
}

SCClique *SideClique::getClique(uint8_t channelIndex) {
    if (channelIndex >= SC_MAX_CLIQUES) return nullptr;
    return cliques_[channelIndex].active ? &cliques_[channelIndex] : nullptr;
}

// ─── Member management ────────────────────────────────────────────────

SCMember *SideClique::findMember(uint32_t nodeNum) {
    // Search across all active cliques
    for (uint8_t c = 0; c < SC_MAX_CLIQUES; c++) {
        if (!cliques_[c].active) continue;
        for (uint8_t i = 0; i < cliques_[c].memberCount; i++) {
            if (cliques_[c].members[i].nodeNum == nodeNum) return &cliques_[c].members[i];
        }
    }
    return nullptr;
}

SCMember *SideClique::findMemberInClique(SCClique &clique, uint32_t nodeNum) {
    for (uint8_t i = 0; i < clique.memberCount; i++) {
        if (clique.members[i].nodeNum == nodeNum) return &clique.members[i];
    }
    return nullptr;
}

SCMember *SideClique::addMember(uint32_t nodeNum, const char *name, uint8_t role) {
    // Add to first active clique (legacy — use addMemberToClique for specific)
    for (uint8_t c = 0; c < SC_MAX_CLIQUES; c++) {
        if (!cliques_[c].active) continue;
        return addMemberToClique(cliques_[c], nodeNum, name, role);
    }
    return nullptr;
}

SCMember *SideClique::addMemberToClique(SCClique &clique, uint32_t nodeNum, const char *name, uint8_t role) {
    SCMember *m = findMemberInClique(clique, nodeNum);
    if (m) return m;
    if (clique.memberCount >= SC_MAX_MEMBERS) return nullptr;

    m = &clique.members[clique.memberCount++];
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
        me->syncSeq = 0; // TODO: per-clique seq

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
        uint32_t seq = 0; memcpy(buf + 15, &seq, 4);
    }

    sendCliquePacket(SC_MSG_BEACON, buf + 1, 18);
    lastBeacon_ = getTime();
}

// ─── Send clique packet on encrypted channel ─────────────────────────

bool SideClique::sendCliquePacket(uint8_t msgType, const uint8_t *data, size_t len, uint32_t dest) {
    // Send on all active clique channels via port 256 (invisible back channel)
    for (uint8_t c = 0; c < SC_MAX_CLIQUES; c++) {
        if (!cliques_[c].active) continue;
        sendCliquePacketOnChannel(msgType, data, len, cliques_[c].channelIndex, dest);
    }
    return true;
}

bool SideClique::sendCliquePacketOnChannel(uint8_t msgType, const uint8_t *data, size_t len, uint8_t channel, uint32_t dest) {
    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p) return false;

    p->to = dest ? dest : NODENUM_BROADCAST;
    p->channel = channel;
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


ProcessMessage SideClique::handleReceived(const meshtastic_MeshPacket &mp) {
    if (!mp.decoded.payload.size) return ProcessMessage::CONTINUE;

    // Scan channels for encrypted cliques on first message
    if (!initialized_) {
        scanCliques();
        if (activeCliques_ == 0) {
            bool isDM = (mp.to == nodeDB->getNodeNum()) && !isBroadcast(mp.to);
            if (isDM) {
                sendReply(mp,
                    "SideClique ERROR:\n"
                    "No encrypted channels found.\n"
                    "Set a PSK on at least one channel\n"
                    "to create a clique.");
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

    // Broadcast/channel message: auto-add sender to the correct clique
    if (mp.from != nodeDB->getNodeNum()) {
        SCClique *clique = getClique(mp.channel);
        if (clique) {
            SCMember *m = findMemberInClique(*clique, mp.from);
            if (!m) {
                m = addMemberToClique(*clique, mp.from, getNodeShortName(mp.from), SC_ROLE_MEMBER);
            }
            if (m) {
                m->lastSeen = getTime();
                const meshtastic_NodeInfoLite *sender = nodeDB->getMeshNode(mp.from);
                if (sender && sender->has_position) {
                    m->latitude = sender->position.latitude_i;
                    m->longitude = sender->position.longitude_i;
                    m->altitude = sender->position.altitude;
                    updateMemberLocation(m);
                }
            }
        }
    }

    // Dispatch protocol messages on port 256 (invisible back channel)
    if (mp.decoded.portnum == meshtastic_PortNum_PRIVATE_APP && len >= 1) {
        uint8_t msgType = (uint8_t)buf[0];
        const uint8_t *payload = (const uint8_t *)buf + 1;
        size_t payloadLen = len - 1;

        switch (msgType) {
            case SC_MSG_BEACON:
                handleBeacon(mp, payload, payloadLen);
                break;
            case SC_MSG_SYNC_REQ:
                handleSyncReq(mp, payload, payloadLen);
                break;
            case SC_MSG_SYNC_DATA:
                handleSyncData(mp, payload, payloadLen);
                break;
            case SC_MSG_DM:
                handleCliqueDM(mp, payload, payloadLen);
                break;
            case SC_MSG_DM_ACK:
                handleDMAck(mp, payload, payloadLen);
                break;
            case SC_MSG_LOCATE:
                handleLocate(mp, payload, payloadLen);
                break;
            case SC_MSG_PING:
                handlePing(mp, payload, payloadLen);
                break;
            case SC_MSG_ALERT:
                handleAlert(mp, payload, payloadLen);
                break;
            case SC_MSG_SOS:
                handleSOS(mp, payload, payloadLen);
                break;
            case SC_MSG_STATUS:
                handleStatusUpdate(mp, payload, payloadLen);
                break;
        }
        return ProcessMessage::STOP;
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
        case SC_STATE_WORDLE:
            return handleStateWordle(mp, session, text);
        case SC_STATE_WASTELAD:
            return handleStateWastelad(mp, session, text);
        case SC_STATE_QUEST:
            return handleStateQuest(mp, session, text);
        case SC_STATE_CHESS:
            return handleStateChess(mp, session, text);
        default:
            session.state = SC_STATE_MAIN;
            sendMainMenu(mp);
            return ProcessMessage::STOP;
    }
}

// ─── Menus ────────────────────────────────────────────────────────────

void SideClique::sendMainMenu(const meshtastic_MeshPacket &req) {
    // Count total members across all cliques
    uint16_t totalMembers = 0;
    for (uint8_t c = 0; c < SC_MAX_CLIQUES; c++)
        if (cliques_[c].active) totalMembers += cliques_[c].memberCount;

    char menu[200];
    snprintf(menu, sizeof(menu),
             "SideClique [%u cliques %u members]\n"
             "[C]heck-in board\n"
             "[D]M send\n"
             "[S]tatus update\n"
             "[P]ing member\n"
             "[F]ind member\n"
             "[!]SOS\n"
             "[R]Wastelad RPG\n"
             "[Q]Daily Quest\n"
             "[W]ordle\n"
             "[K]Chess by Mesh\n"
             "[X]Exit",
             activeCliques_, totalMembers);
    sendReply(req, menu);
}

void SideClique::sendCheckinBoard(const meshtastic_MeshPacket &req) {
    char board[512] = "";
    uint32_t now = getTime();

    for (uint8_t c = 0; c < SC_MAX_CLIQUES; c++) {
        if (!cliques_[c].active || cliques_[c].memberCount == 0) continue;
        char hdr[32];
        snprintf(hdr, sizeof(hdr), "=== %s ===\n", cliques_[c].name);
        strncat(board, hdr, sizeof(board) - strlen(board) - 1);

    for (uint8_t i = 0; i < cliques_[c].memberCount; i++) {
        SCMember &m = cliques_[c].members[i];
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

    } // end clique loop

    if (activeCliques_ == 0) {
        strncat(board, "(no cliques active)\n", sizeof(board) - strlen(board) - 1);
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
            // Ping a member — next message should be their name
            sendReply(mp, "Ping who? Enter name:");
            // For now just show checkin board as ping response
            // TODO: add SC_STATE_PING_TARGET state
            sendCheckinBoard(mp);
            break;
        }
        case 'f': {
            // Find/LOCATE a member
            sendReply(mp, "LOCATE who? Enter name:\n(Will broadcast their GPS every 60s)");
            // TODO: add SC_STATE_LOCATE_TARGET state
            // For now, broadcast locate for all members as a demo
            break;
        }
        case 'r': {
            // Wastelad RPG (full TinyBBS version)
            session.state = SC_STATE_WASTELAD;
            frpgEnsureDir();
            char rpgBuf[512];
            bool exitGame = false;
            frpgCommand(mp.from, "", getNodeShortName(mp.from), rpgBuf, sizeof(rpgBuf), exitGame);
            sendReply(mp, rpgBuf);
            break;
        }
        case 'q':
            doQuestStart(mp, session);
            break;
        case 'w':
            doWordleStart(mp, session);
            break;
        case 'k':
            chessEnsureDir();
            session.state = SC_STATE_CHESS;
            session.chessGameId = 0;
            sendChessStatus(mp, 0);
            break;
        case '!': {
            // SOS — check if followed by a name (remote SOS) or standalone (self SOS)
            const char *target = text + 1;
            while (*target == ' ') target++;
            if (*target) {
                // Remote SOS: "! Alex" → send SOS to Alex's node
                uint32_t targetNode = 0;
                for (uint8_t c = 0; c < SC_MAX_CLIQUES && !targetNode; c++) {
                    if (!cliques_[c].active) continue;
                    for (uint8_t i = 0; i < cliques_[c].memberCount; i++) {
                        if (strncasecmp(cliques_[c].members[i].name, target, strlen(target)) == 0) {
                            targetNode = cliques_[c].members[i].nodeNum;
                            break;
                        }
                    }
                }
                if (targetNode) {
                    uint8_t sosBuf[4];
                    memcpy(sosBuf, &targetNode, 4);
                    sendCliquePacket(SC_MSG_SOS, sosBuf, 4);
                    SCMember *tm = findMember(targetNode);
                    char reply[80];
                    snprintf(reply, sizeof(reply), "Remote SOS sent to %s\nTheir node will broadcast position",
                             tm ? tm->name : "???");
                    sendReply(mp, reply);
                } else {
                    sendReply(mp, "Member not found for remote SOS.");
                }
            } else {
                // Self SOS
                sosMode_ = true;
                locateMode_ = true;
                SCMember *me = findMember(nodeDB->getNodeNum());
                if (me) me->status = SC_STATUS_SOS;
                sendBeacon();
                sendReply(mp, "SOS ACTIVATED\nBroadcasting position every 30s\nAll members alerted\n\nSend !cancel to stop");
            }
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

    // Find member by name or node ID across all cliques
    uint32_t targetNode = 0;
    for (uint8_t c = 0; c < SC_MAX_CLIQUES && !targetNode; c++) {
        if (!cliques_[c].active) continue;
        for (uint8_t i = 0; i < cliques_[c].memberCount; i++) {
            if (strncasecmp(cliques_[c].members[i].name, text, strlen(text)) == 0) {
                targetNode = cliques_[c].members[i].nodeNum;
                break;
            }
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
    static uint32_t dmSeqCounter = 0; dm.id = ++dmSeqCounter;
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

// ─── Protocol handlers (Phase 2) ──────────────────────────────────────

void SideClique::handleBeacon(const meshtastic_MeshPacket &mp, const uint8_t *data, size_t len) {
    if (len < 18 || mp.from == nodeDB->getNodeNum()) return;

    SCClique *clique = getClique(mp.channel);
    if (!clique) return;

    // Parse beacon: status(1) + battery(1) + lat(4) + lon(4) + alt(4) + seq(4)
    uint8_t status = data[0];
    uint8_t battery = data[1];
    int32_t lat, lon, alt;
    uint32_t seq;
    memcpy(&lat, data + 2, 4);
    memcpy(&lon, data + 6, 4);
    memcpy(&alt, data + 10, 4);
    memcpy(&seq, data + 14, 4);

    SCMember *m = findMemberInClique(*clique, mp.from);
    if (!m) m = addMemberToClique(*clique, mp.from, getNodeShortName(mp.from), SC_ROLE_MEMBER);
    if (!m) return;

    m->status = status;
    m->battery = battery;
    m->latitude = lat;
    m->longitude = lon;
    m->altitude = alt;
    m->syncSeq = seq;
    m->lastSeen = getTime();
    updateMemberLocation(m);

    // If this member has pending DMs, send them now
    retryPendingDMs(getTime());
}

void SideClique::handleSyncReq(const meshtastic_MeshPacket &mp, const uint8_t *data, size_t len) {
    // TODO Phase 2: vector clock comparison + delta sync
    (void)mp; (void)data; (void)len;
}

void SideClique::handleSyncData(const meshtastic_MeshPacket &mp, const uint8_t *data, size_t len) {
    // TODO Phase 2: receive and store synced messages
    (void)mp; (void)data; (void)len;
}

void SideClique::handleCliqueDM(const meshtastic_MeshPacket &mp, const uint8_t *data, size_t len) {
    if (len < 5) return; // min: to(4) + text(1)

    uint32_t toNode;
    memcpy(&toNode, data, 4);

    // Is this DM for us?
    if (toNode == nodeDB->getNodeNum()) {
        // Deliver to our inbox
        char text[160] = {0};
        size_t textLen = len - 4;
        if (textLen > 159) textLen = 159;
        memcpy(text, data + 4, textLen);

        const char *fromName = getNodeShortName(mp.from);
        char notify[200];
        snprintf(notify, sizeof(notify), "DM from %s:\n%s",
                 fromName ? fromName : "???", text);

        // Send ACK back
        uint8_t ackData[4];
        memcpy(ackData, &mp.id, 4); // echo the message ID
        sendCliquePacketOnChannel(SC_MSG_DM_ACK, ackData, 4, mp.channel, mp.from);

        // TODO: store in inbox, notify via screen/buzzer
        LOG_INFO("[SC] DM from %s: %s\n", fromName ? fromName : "???", text);
    } else {
        // Not for us — store for relay if we see the target later
        // TODO Phase 2: relay queue
    }
}

void SideClique::handleDMAck(const meshtastic_MeshPacket &mp, const uint8_t *data, size_t len) {
    if (len < 4) return;
    uint32_t ackedMsgId;
    memcpy(&ackedMsgId, data, 4);

    // Mark matching queued DM as delivered
    for (uint8_t i = 0; i < dmQueueCount_; i++) {
        if (dmQueue_[i].to == mp.from && dmQueue_[i].status < 2) {
            dmQueue_[i].status = 2; // delivered
            LOG_INFO("[SC] DM to %08x delivered\n", mp.from);
            break;
        }
    }
}

void SideClique::handleStatusUpdate(const meshtastic_MeshPacket &mp, const uint8_t *data, size_t len) {
    if (len < 1) return;
    uint8_t newStatus = data[0];

    // Update member status in all cliques they're in
    for (uint8_t c = 0; c < SC_MAX_CLIQUES; c++) {
        if (!cliques_[c].active) continue;
        SCMember *m = findMemberInClique(cliques_[c], mp.from);
        if (m) {
            m->status = newStatus;
            m->lastSeen = getTime();
        }
    }
}

// ─── Remote commands ──────────────────────────────────────────────────

void SideClique::handleLocate(const meshtastic_MeshPacket &mp, const uint8_t *data, size_t len) {
    if (len < 4) return;

    uint32_t targetNode;
    memcpy(&targetNode, data, 4);

    if (targetNode != nodeDB->getNodeNum()) return; // not for us

    // Check if sender has ADMIN role (parental control)
    SCMember *sender = findMember(mp.from);
    bool isAdmin = sender && sender->role == SC_ROLE_ADMIN;

    // Activate locate mode — broadcast GPS every 60s
    locateMode_ = true;
    LOG_INFO("[SC] LOCATE activated by %08x (admin=%d)\n", mp.from, isAdmin);

    // Send immediate position update
    sendBeacon();
}

void SideClique::handlePing(const meshtastic_MeshPacket &mp, const uint8_t *data, size_t len) {
    (void)data; (void)len;

    // Respond with status immediately
    SCMember *me = findMember(nodeDB->getNodeNum());
    if (!me) return;

    char reply[120];
    snprintf(reply, sizeof(reply), "%s: %s | %u%% | %s | Up | Last:%lus ago",
             me->name, statusStr(me->status), me->battery,
             me->location[0] ? me->location : "no GPS",
             (unsigned long)(getTime() - me->lastSeen));

    // Reply on visible channel so sender sees it
    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p) return;
    p->to = mp.from;
    p->channel = mp.channel;
    p->want_ack = false;
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP; // visible reply
    size_t rlen = strlen(reply);
    if (rlen > 200) rlen = 200;
    p->decoded.payload.size = rlen;
    memcpy(p->decoded.payload.bytes, reply, rlen);
    service->sendToMesh(p);
}

void SideClique::handleAlert(const meshtastic_MeshPacket &mp, const uint8_t *data, size_t len) {
    if (len < 4) return;

    uint32_t targetNode;
    memcpy(&targetNode, data, 4);

    if (targetNode != nodeDB->getNodeNum()) return; // not for us

    // Extract message
    char alertMsg[160] = {0};
    if (len > 4) {
        size_t msgLen = len - 4;
        if (msgLen > 159) msgLen = 159;
        memcpy(alertMsg, data + 4, msgLen);
    }

    const char *fromName = getNodeShortName(mp.from);
    LOG_INFO("[SC] ALERT from %s: %s\n", fromName ? fromName : "???", alertMsg);

    // TODO: activate buzzer/LED, display persistently on screen
}

void SideClique::handleSOS(const meshtastic_MeshPacket &mp, const uint8_t *data, size_t len) {
    if (len < 1) return;

    // SOS can be self-triggered (target=0) or remote (target=nodeNum)
    uint32_t targetNode = 0;
    if (len >= 4) memcpy(&targetNode, data, 4);

    if (targetNode == 0 || targetNode == nodeDB->getNodeNum()) {
        // SOS is for us — activate
        SCMember *sender = findMember(mp.from);
        bool isAdmin = sender && sender->role == SC_ROLE_ADMIN;

        sosMode_ = true;
        locateMode_ = true;

        // Update our status
        SCMember *me = findMember(nodeDB->getNodeNum());
        if (me) me->status = SC_STATUS_SOS;

        LOG_INFO("[SC] SOS activated by %08x (admin=%d, remote=%d)\n",
                 mp.from, isAdmin, targetNode != 0);

        // Broadcast SOS beacon immediately
        sendBeacon();

        // Announce on visible channel
        const char *myName = getNodeShortName(nodeDB->getNodeNum());
        char announce[120];
        snprintf(announce, sizeof(announce), "SOS: %s emergency!\nBroadcasting position every 30s",
                 myName ? myName : "???");

        meshtastic_MeshPacket *p = allocDataPacket();
        if (p) {
            p->to = NODENUM_BROADCAST;
            p->channel = mp.channel;
            p->want_ack = false;
            p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP; // visible
            size_t alen = strlen(announce);
            p->decoded.payload.size = alen;
            memcpy(p->decoded.payload.bytes, announce, alen);
            service->sendToMesh(p);
        }
    }
    // If SOS is for someone else on the mesh, relay it
    // (handled by Meshtastic's mesh routing automatically)
}

// ─── Remote command senders (from user menu) ──────────────────────────

static void buildLocatePacket(uint32_t targetNode, uint8_t *buf) {
    memcpy(buf, &targetNode, 4);
}

static void buildSOSPacket(uint32_t targetNode, uint8_t *buf) {
    memcpy(buf, &targetNode, 4);
}

static void buildAlertPacket(uint32_t targetNode, const char *msg, uint8_t *buf, size_t *outLen) {
    memcpy(buf, &targetNode, 4);
    size_t msgLen = strlen(msg);
    if (msgLen > 155) msgLen = 155;
    memcpy(buf + 4, msg, msgLen);
    *outLen = 4 + msgLen;
}

static void buildPingPacket(uint32_t targetNode, uint8_t *buf) {
    memcpy(buf, &targetNode, 4);
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

    // Check canary for all members across all cliques
    for (uint8_t c = 0; c < SC_MAX_CLIQUES; c++) {
        if (!cliques_[c].active) continue;
    for (uint8_t i = 0; i < cliques_[c].memberCount; i++) {
        SCMember &m = cliques_[c].members[i];
        if (m.nodeNum == nodeDB->getNodeNum()) continue;
        if (m.status != SC_STATUS_SOS && m.status != SC_STATUS_UNKNOWN) {
            if (t - m.lastSeen > SC_CANARY_TIMEOUT_S) {
                m.status = SC_STATUS_UNKNOWN;
                // TODO: alert admin members
            }
        }
    } // end member loop
    } // end clique loop

    return sosMode_ ? 10000 : 30000; // poll faster during SOS
}

// ─── Storage (placeholder — Phase 2 will use external flash) ──────────

void SideClique::loadState() {}
void SideClique::saveState() {}
void SideClique::loadDMQueue() {}
void SideClique::saveDMQueue() {}

// ─── Wastelad RPG ─────────────────────────────────────────────────────

ProcessMessage SideClique::handleStateWastelad(const meshtastic_MeshPacket &mp, SCSession &session, const char *text) {
    if (!text || text[0] == '\0') {
        session.state = SC_STATE_MAIN;
        sendMainMenu(mp);
        return ProcessMessage::STOP;
    }

    char rpgBuf[512];
    bool exitGame = false;
    frpgCommand(mp.from, text, getNodeShortName(mp.from), rpgBuf, sizeof(rpgBuf), exitGame);

    if (exitGame) {
        session.state = SC_STATE_MAIN;
        sendReply(mp, rpgBuf);
        sendMainMenu(mp);
    } else {
        sendReply(mp, rpgBuf);
    }
    return ProcessMessage::STOP;
}

// ─── Wordle ───────────────────────────────────────────────────────────

uint32_t SideClique::wordleDay() {
    uint32_t t = getTime();
    int64_t adj = (int64_t)t - 9 * 3600; // 9am UTC boundary
    if (adj < 0) return 0;
    return (uint32_t)(adj / 86400);
}

void SideClique::doWordleStart(const meshtastic_MeshPacket &req, SCSession &session) {
    uint32_t day = wordleDay();
    const char *word = wordlePickWord(day);
    strncpy(session.wordleTarget, word, 5);
    session.wordleTarget[5] = '\0';
    session.wordleGuesses = 0;
    session.wordleDay = day;
    session.state = SC_STATE_WORDLE;
    sendReply(req, "=== Wordle ===\nGuess a 5-letter word!\n6 tries. Feedback:\nG=right Y=wrong spot X=no\n\nGuess 1/6:");
}

ProcessMessage SideClique::handleStateWordle(const meshtastic_MeshPacket &mp, SCSession &session, const char *text) {
    if (!text || text[0] == '\0') {
        sendReply(mp, "Enter a 5-letter word:");
        return ProcessMessage::STOP;
    }
    if (tolower((unsigned char)text[0]) == 'x' || tolower((unsigned char)text[0]) == 'q') {
        char msg[60];
        snprintf(msg, sizeof(msg), "Quit. Word was: %s", session.wordleTarget);
        sendReply(mp, msg);
        session.state = SC_STATE_MAIN;
        sendMainMenu(mp);
        return ProcessMessage::STOP;
    }

    char guess[6] = {0};
    int guessLen = 0;
    for (int i = 0; text[i] && guessLen < 5; i++) {
        if (isalpha((unsigned char)text[i]))
            guess[guessLen++] = tolower((unsigned char)text[i]);
    }
    if (guessLen != 5) {
        sendReply(mp, "Enter a 5-letter word.");
        return ProcessMessage::STOP;
    }

    if (!wordleIsValid(guess)) {
        sendReply(mp, "Not valid. Try again.");
        return ProcessMessage::STOP;
    }

    session.wordleGuesses++;

    char fb[6] = {0};
    wordleFeedback(guess, session.wordleTarget, fb);

    char fbStr[16];
    snprintf(fbStr, sizeof(fbStr), "%c%c%c%c%c %c%c%c%c%c",
             guess[0],guess[1],guess[2],guess[3],guess[4],
             fb[0],fb[1],fb[2],fb[3],fb[4]);

    char reply[120];
    if (strncmp(fb, "GGGGG", 5) == 0) {
        snprintf(reply, sizeof(reply), "%s\nYou got it in %u/6!", fbStr, session.wordleGuesses);
        sendReply(mp, reply);
        session.state = SC_STATE_MAIN;
    } else if (session.wordleGuesses >= 6) {
        snprintf(reply, sizeof(reply), "%s\nGame over! Word: %s", fbStr, session.wordleTarget);
        sendReply(mp, reply);
        session.state = SC_STATE_MAIN;
    } else {
        snprintf(reply, sizeof(reply), "%s  Guess %u/6:", fbStr, session.wordleGuesses + 1);
        sendReply(mp, reply);
    }
    return ProcessMessage::STOP;
}

// ─── Chess ────────────────────────────────────────────────────────────

void SideClique::sendChessStatus(const meshtastic_MeshPacket &req, uint32_t gameId) {
    // Simple chess menu for now
    sendReply(req,
              "=== Chess by Mesh ===\n"
              "NW-New White NB-New Black\n"
              "MV-Move (e.g. MV e2e4)\n"
              "LB-Board AB-About\n"
              "[X]Back to SideClique");
}

ProcessMessage SideClique::handleStateChess(const meshtastic_MeshPacket &mp, SCSession &session, const char *text) {
    if (!text || text[0] == '\0') {
        sendChessStatus(mp, session.chessGameId);
        return ProcessMessage::STOP;
    }

    char cmd[8] = {0};
    strncpy(cmd, text, 7);
    for (char *p = cmd; *p; p++) *p = toupper((unsigned char)*p);

    if (strcmp(cmd, "X") == 0) {
        session.state = SC_STATE_MAIN;
        sendMainMenu(mp);
        return ProcessMessage::STOP;
    }

    char reply[200] = {0};

    if (strncmp(cmd, "NW", 2) == 0 || strncmp(cmd, "NB", 2) == 0) {
        // New game
        BBSChessGame game;
        memset(&game, 0, sizeof(game));
        game.id = chessNextGameId();
        game.whiteNode = (cmd[1] == 'W') ? mp.from : 0;
        game.blackNode = (cmd[1] == 'B') ? mp.from : 0;
        game.difficulty = 1; // medium
        game.toMove = 0; // white
        game.status = 0;
        game.castling = 0x0F; // all castling available
        game.enPassantFile = -1;
        chessBoardInit(game.board);
        chessSaveGame(game);
        session.chessGameId = game.id;

        // If AI goes first (player is black), make AI move
        if (game.blackNode == 0 && cmd[1] == 'B') {
            // AI is white, goes first
            char aiMove[8];
            if (chessAIMove(game, aiMove)) {
                chessApplyMove(game, aiMove);
                chessSaveGame(game);
            }
        }
        chessBuildBoard(game, reply, sizeof(reply), cmd[1] == 'W');
        sendReply(mp, reply);
    }
    else if (strncmp(cmd, "MV", 2) == 0) {
        // Move
        if (session.chessGameId == 0) {
            sendReply(mp, "No active game. NW or NB to start.");
            return ProcessMessage::STOP;
        }
        BBSChessGame game;
        if (!chessLoadGame(session.chessGameId, game)) {
            sendReply(mp, "Game not found.");
            return ProcessMessage::STOP;
        }
        const char *move = text + 2;
        while (*move == ' ') move++;
        if (!chessApplyMove(game, move)) {
            sendReply(mp, "Invalid move. Use format: e2e4");
            return ProcessMessage::STOP;
        }
        // AI response
        char aiMove[8];
        if (game.status == 0 && chessAIMove(game, aiMove)) {
            chessApplyMove(game, aiMove);
        }
        chessSaveGame(game);
        chessBuildBoard(game, reply, sizeof(reply), game.whiteNode == mp.from);
        sendReply(mp, reply);
    }
    else if (strncmp(cmd, "LB", 2) == 0) {
        // Show board
        if (session.chessGameId == 0) {
            sendReply(mp, "No active game.");
            return ProcessMessage::STOP;
        }
        BBSChessGame game;
        if (chessLoadGame(session.chessGameId, game)) {
            chessBuildBoard(game, reply, sizeof(reply), game.whiteNode == mp.from);
            sendReply(mp, reply);
        }
    }
    else if (strncmp(cmd, "AB", 2) == 0) {
        sendReply(mp, "Chess by Mesh\nAlpha-beta AI\nMV e2e4 to move\nNW=play white NB=black");
    }
    else {
        sendChessStatus(mp, session.chessGameId);
    }

    return ProcessMessage::STOP;
}

// ─── Daily Quest (LORD-style RPG) ─────────────────────────────────────

void SideClique::doQuestStart(const meshtastic_MeshPacket &req, SCSession &session) {
    DQPlayer &p = session.questPlayer;
    uint32_t today = wordleDay();

    // Initialize new player
    if (p.level == 0) {
        p.nodeNum = req.from;
        const char *sn = getNodeShortName(req.from);
        if (sn) strncpy(p.name, sn, sizeof(p.name) - 1);
        else snprintf(p.name, sizeof(p.name), "Hero");
        p.level = 1;
        p.xp = 0;
        p.caps = 10;
        p.weapon = 0;
        p.armor = 0;
        p.stimpaks = DQ_MAX_STIMPAKS;
    }

    // Reset daily actions if new day
    if (p.lastPlayDay != today) {
        p.lastPlayDay = today;
        p.actionsUsed = 0;
        p.questHp = dqPlayerMaxHp(p);
        p.inCombat = 0;
        p.monsterIdx = 0;
        p.stimpaks = DQ_MAX_STIMPAKS;
    }

    session.state = SC_STATE_QUEST;

    char reply[200];
    snprintf(reply, sizeof(reply),
             "=== %s ===\n"
             "%s Lvl:%u HP:%u/%u\n"
             "ATK:%u DEF:%u Gold:%u\n"
             "XP:%u/%u Actions:%u/%u\n"
             "[E]xplore [S]hop\n"
             "[L]eaderboard [X]Back",
             dqGetLocation(today),
             p.name, p.level, p.questHp, dqPlayerMaxHp(p),
             dqPlayerAtk(p), dqPlayerDef(p), p.caps,
             p.xp, (p.level < DQ_MAX_LEVEL) ? DQ_XP_THRESH[p.level + 1] : 9999,
             p.actionsUsed, DQ_MAX_ACTIONS);
    sendReply(req, reply);
}

void SideClique::doQuestExplore(const meshtastic_MeshPacket &req, SCSession &session) {
    DQPlayer &p = session.questPlayer;
    uint32_t today = wordleDay();

    if (p.actionsUsed >= DQ_MAX_ACTIONS) {
        sendReply(req, "No actions left today!\nCome back tomorrow, Wastelad.");
        return;
    }
    if (p.questHp == 0) {
        sendReply(req, "You're knocked out!\nRest until tomorrow.");
        return;
    }

    p.actionsUsed++;
    p.monsterIdx++;

    // Determine event type
    uint8_t event = dqGetEventType(today, p.monsterIdx, p.level);
    p.eventType = event;

    char reply[200];
    uint32_t rng = dqDaySeed(today + p.monsterIdx * 137);

    switch (event) {
        case DQ_EVENT_CHENG: {
            // Chairman Cheng — final boss
            p.monsterHp = (uint8_t)(DQ_CHENG.baseHp * (0.8f + p.level * 0.1f));
            p.inCombat = 1;
            snprintf(reply, sizeof(reply),
                     "CHAIRMAN CHENG appears!\n"
                     "\"You dare challenge me?\"\n"
                     "HP:%u ATK:%u\n"
                     "Your HP:%u/%u\n"
                     "[A]ttack [D]efend [F]lee",
                     p.monsterHp, (uint8_t)(DQ_CHENG.baseAtk * (0.8f + p.level * 0.1f)),
                     p.questHp, dqPlayerMaxHp(p));
            break;
        }
        case DQ_EVENT_STRANGER: {
            // Mysterious Stranger — free weapon upgrade
            if (p.weapon < 5) {
                p.weapon++;
                snprintf(reply, sizeof(reply),
                         "The Mysterious Stranger\nappears from the fog!\n"
                         "He hands you a %s\nand vanishes.\n"
                         "ATK is now %u",
                         DQ_WEAPONS[p.weapon], dqPlayerAtk(p));
            } else {
                p.caps += 100;
                snprintf(reply, sizeof(reply),
                         "The Mysterious Stranger\ntips his hat and drops\n"
                         "a bag of caps. +100 caps");
            }
            break;
        }
        case DQ_EVENT_RAIDER: {
            // Raider ambush — lose caps or fight
            uint16_t stolen = p.caps / 4;
            if (stolen < 5) stolen = 5;
            snprintf(reply, sizeof(reply),
                     "Raider ambush!\n"
                     "\"Hand over %u caps or\nwe'll take 'em!\"\n"
                     "[P]ay %u caps\n"
                     "[F]ight the raider",
                     stolen, stolen);
            p.monsterHp = 20 + p.level * 3;
            // Store stolen amount in monsterIdx temporarily
            break;
        }
        case DQ_EVENT_BOS: {
            // Brotherhood of Steel patrol — heal and buff
            uint8_t heal = dqPlayerMaxHp(p) / 2;
            p.questHp += heal;
            if (p.questHp > dqPlayerMaxHp(p)) p.questHp = dqPlayerMaxHp(p);
            p.stimpaks++;
            snprintf(reply, sizeof(reply),
                     "Brotherhood patrol!\n"
                     "\"Ad Victoriam, soldier.\"\n"
                     "They patch you up.\n"
                     "+%u HP, +1 Stimpak\n"
                     "HP:%u/%u",
                     heal, p.questHp, dqPlayerMaxHp(p));
            break;
        }
        case DQ_EVENT_VAULT_REP: {
            // Vault-Tec Rep — riddle for XP
            uint8_t ridIdx = rng % DQ_RIDDLE_COUNT;
            p.eventType = DQ_EVENT_VAULT_REP;
            p.monsterIdx = ridIdx; // store riddle index
            snprintf(reply, sizeof(reply),
                     "Vault-Tec Rep!\n"
                     "\"Got a minute? Quiz!\"\n"
                     "%s",
                     DQ_RIDDLES[ridIdx].question);
            break;
        }
        case DQ_EVENT_TAVERN: {
            // Wasteland Tavern
            uint8_t quoteIdx = rng % DQ_TAVERN_QUOTE_COUNT;
            snprintf(reply, sizeof(reply),
                     "=== Wasteland Tavern ===\n"
                     "%s\n"
                     "[G]amble 20 caps\n"
                     "[D]rink (heal 10HP, 5caps)\n"
                     "[L]eave",
                     DQ_TAVERN_QUOTES[quoteIdx]);
            break;
        }
        default: {
            // Regular combat
            DQMonster mon;
            uint8_t monHp, monAtk;
            dqGetMonster(today, p.monsterIdx, p.level, mon, monHp, monAtk);
            p.monsterHp = monHp;
            p.inCombat = 1;
            snprintf(reply, sizeof(reply),
                     "A %s appears! (HP:%u ATK:%u)\n"
                     "Your HP:%u/%u\n"
                     "[A]ttack [D]efend\n"
                     "[F]lee [S]timpak(%u)",
                     mon.name, monHp, monAtk,
                     p.questHp, dqPlayerMaxHp(p),
                     p.stimpaks);
            break;
        }
    }
    sendReply(req, reply);
}

void SideClique::doQuestCombat(const meshtastic_MeshPacket &req, SCSession &session, char action) {
    DQPlayer &p = session.questPlayer;
    uint32_t today = wordleDay();

    if (!p.inCombat) {
        doQuestExplore(req, session);
        return;
    }

    DQMonster mon;
    uint8_t monMaxHp, monAtk;
    dqGetMonster(today, p.monsterIdx - 1, p.level, mon, monMaxHp, monAtk);

    char reply[200];
    uint8_t playerAtk = dqPlayerAtk(p);
    uint8_t playerDef = dqPlayerDef(p);

    switch (action) {
        case 'a': {
            // Attack
            uint8_t dmg = playerAtk > monAtk/2 ? playerAtk - monAtk/4 : 1;
            // Add some randomness from day seed
            uint32_t rng = dqDaySeed(today + p.actionsUsed * 137);
            dmg = dmg * (80 + (rng % 41)) / 100; // 80-120% damage
            if (dmg < 1) dmg = 1;

            uint8_t monDmg = monAtk > playerDef ? monAtk - playerDef/2 : 1;
            monDmg = monDmg * (80 + ((rng >> 8) % 41)) / 100;
            if (monDmg < 1) monDmg = 1;

            if (p.monsterHp <= dmg) {
                // Monster defeated!
                p.monsterHp = 0;
                p.inCombat = 0;
                p.xp += mon.xpReward;
                p.caps += mon.capsReward;

                bool leveled = dqCheckLevelUp(p);
                snprintf(reply, sizeof(reply),
                         "You hit for %u! %s defeated!\n"
                         "+%uXP +%uGold%s\n"
                         "HP:%u/%u Actions:%u/%u",
                         dmg, mon.name, mon.xpReward, mon.capsReward,
                         leveled ? "\nLEVEL UP!" : "",
                         p.questHp, dqPlayerMaxHp(p),
                         p.actionsUsed, DQ_MAX_ACTIONS);
                if (leveled) p.questHp = dqPlayerMaxHp(p);
            } else {
                p.monsterHp -= dmg;
                if (p.questHp <= monDmg) {
                    p.questHp = 0;
                    p.inCombat = 0;
                    snprintf(reply, sizeof(reply),
                             "You hit for %u! %s hits back for %u!\n"
                             "You are knocked out!\nRest until tomorrow.",
                             dmg, mon.name, monDmg);
                } else {
                    p.questHp -= monDmg;
                    snprintf(reply, sizeof(reply),
                             "You:%u dmg %s:%u dmg\n"
                             "%s HP:%u You HP:%u/%u\n"
                             "[A]tk [D]ef [F]lee [P]ot(%u)",
                             dmg, mon.name, monDmg,
                             mon.name, p.monsterHp, p.questHp, dqPlayerMaxHp(p),
                             p.stimpaks);
                }
            }
            break;
        }
        case 'd': {
            // Defend — take half damage, no attack
            uint8_t monDmg = monAtk > playerDef * 2 ? (monAtk - playerDef * 2) / 2 : 0;
            if (monDmg < 1) monDmg = 0;
            if (p.questHp > monDmg) p.questHp -= monDmg;
            else { p.questHp = 0; p.inCombat = 0; }

            snprintf(reply, sizeof(reply),
                     "You defend! -%u HP\n"
                     "%s HP:%u You HP:%u/%u\n"
                     "[A]tk [D]ef [F]lee [P]ot(%u)",
                     monDmg, mon.name, p.monsterHp,
                     p.questHp, dqPlayerMaxHp(p), p.stimpaks);
            break;
        }
        case 'f': {
            // Flee — 70% success
            uint32_t rng = dqDaySeed(today + p.actionsUsed * 271);
            if ((rng % 100) < 70) {
                p.inCombat = 0;
                snprintf(reply, sizeof(reply), "You fled!\nHP:%u/%u Actions:%u/%u\n[E]xplore [S]hop [X]Back",
                         p.questHp, dqPlayerMaxHp(p), p.actionsUsed, DQ_MAX_ACTIONS);
            } else {
                uint8_t monDmg = monAtk > playerDef ? monAtk - playerDef/2 : 1;
                if (p.questHp > monDmg) p.questHp -= monDmg;
                else { p.questHp = 0; p.inCombat = 0; }
                snprintf(reply, sizeof(reply), "Flee failed! %s hits for %u!\nHP:%u/%u\n[A]tk [D]ef [F]lee [P]ot(%u)",
                         mon.name, monDmg, p.questHp, dqPlayerMaxHp(p), p.stimpaks);
            }
            break;
        }
        case 'p': {
            // Stimpak
            if (p.stimpaks == 0) {
                snprintf(reply, sizeof(reply), "No potions left!\n[A]tk [D]ef [F]lee");
            } else {
                p.stimpaks--;
                uint8_t heal = dqPlayerMaxHp(p) / 3;
                p.questHp += heal;
                if (p.questHp > dqPlayerMaxHp(p)) p.questHp = dqPlayerMaxHp(p);
                snprintf(reply, sizeof(reply), "Healed %u HP!\nHP:%u/%u Stimpaks:%u\n[A]tk [D]ef [F]lee [P]ot(%u)",
                         heal, p.questHp, dqPlayerMaxHp(p), p.stimpaks, p.stimpaks);
            }
            break;
        }
        default:
            snprintf(reply, sizeof(reply), "[A]ttack [D]efend [F]lee [P]otion(%u)", p.stimpaks);
            break;
    }
    sendReply(req, reply);
}

ProcessMessage SideClique::handleStateQuest(const meshtastic_MeshPacket &mp, SCSession &session, const char *text) {
    if (!text || text[0] == '\0') {
        doQuestStart(mp, session);
        return ProcessMessage::STOP;
    }

    char cmd = tolower((unsigned char)text[0]);

    if (cmd == 'x') {
        session.state = SC_STATE_MAIN;
        sendMainMenu(mp);
        return ProcessMessage::STOP;
    }

    DQPlayer &p = session.questPlayer;

    if (cmd == 'e' && !p.inCombat) {
        doQuestExplore(mp, session);
    } else if (cmd == 's' && !p.inCombat) {
        // Shop
        char reply[200];
        uint16_t wpnCost = (p.weapon + 1) * 50;
        uint16_t armCost = (p.armor + 1) * 40;
        snprintf(reply, sizeof(reply),
                 "=== Shop ===\n"
                 "Gold: %u\n"
                 "1.%s→%s (%ug)\n"
                 "2.%s→%s (%ug)\n"
                 "3.Stimpak (15g)\n"
                 "[X]Back",
                 p.caps,
                 DQ_WEAPONS[p.weapon], p.weapon < 5 ? DQ_WEAPONS[p.weapon + 1] : "MAX", wpnCost,
                 DQ_ARMORS[p.armor], p.armor < 5 ? DQ_ARMORS[p.armor + 1] : "MAX", armCost);
        sendReply(mp, reply);
    } else if (cmd == '1' && !p.inCombat) {
        uint16_t cost = (p.weapon + 1) * 50;
        if (p.weapon >= 5) sendReply(mp, "Weapon is maxed!");
        else if (p.caps < cost) sendReply(mp, "Not enough gold!");
        else { p.caps -= cost; p.weapon++; char r[60]; snprintf(r, sizeof(r), "Bought %s! ATK:%u", DQ_WEAPONS[p.weapon], dqPlayerAtk(p)); sendReply(mp, r); }
    } else if (cmd == '2' && !p.inCombat) {
        uint16_t cost = (p.armor + 1) * 40;
        if (p.armor >= 5) sendReply(mp, "Armor is maxed!");
        else if (p.caps < cost) sendReply(mp, "Not enough gold!");
        else { p.caps -= cost; p.armor++; char r[60]; snprintf(r, sizeof(r), "Bought %s! DEF:%u", DQ_ARMORS[p.armor], dqPlayerDef(p)); sendReply(mp, r); }
    } else if (cmd == '3' && !p.inCombat) {
        if (p.caps < 15) sendReply(mp, "Not enough gold!");
        else { p.caps -= 15; p.stimpaks++; char r[40]; snprintf(r, sizeof(r), "Bought potion! (%u)", p.stimpaks); sendReply(mp, r); }
    } else if (cmd == 'l') {
        // Leaderboard — show quest players from all sessions
        // TODO: sync leaderboard across clique via gossip
        char reply[200];
        snprintf(reply, sizeof(reply), "=== Leaderboard ===\n%s Lvl:%u XP:%u Gold:%u\n(Sync coming soon)",
                 p.name, p.level, p.xp, p.caps);
        sendReply(mp, reply);
    } else if (p.inCombat) {
        doQuestCombat(mp, session, cmd);
    } else {
        doQuestStart(mp, session);
    }

    return ProcessMessage::STOP;
}

// ─── UI Frame ─────────────────────────────────────────────────────────

#if HAS_SCREEN
#include "graphics/ScreenFonts.h"
void SideClique::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) {
    display->setFont(FONT_SMALL);
    display->setColor(OLEDDISPLAY_COLOR::BLACK);
    display->drawString(x, y, "SClq");
    display->setColor(OLEDDISPLAY_COLOR::WHITE);

    uint16_t totalMem = 0;
    for (uint8_t c = 0; c < SC_MAX_CLIQUES; c++)
        if (cliques_[c].active) totalMem += cliques_[c].memberCount;
    char line[32];
    snprintf(line, sizeof(line), "%u clq %u mem", activeCliques_, totalMem);
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
