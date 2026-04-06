# Matchmaking (NpMatching2)

Room-based matchmaking system. Clients create or join rooms, exchange peer information for P2P connectivity, and synchronize room state via binary attributes. All matchmaking state is held in memory — rooms are not persisted across server restarts.

---

## Overview

Matchmaking follows the NpMatching2 model: clients register callback handlers, then create or join rooms. When a new member joins, the server automatically exchanges signaling information between all room members so they can establish peer-to-peer connections. Room owners can update room attributes that are either broadcast to members (internal) or made available for room discovery (external/searchable).

---

## Data Structures

### MatchingSessionState (per-session)

Each authenticated `ClientSession` holds a `MatchingSessionState` that tracks the client's matchmaking context.

| Field | Type | Description |
|---|---|---|
| `addr` | string | Client's UDP endpoint address (from RegisterHandlers or TCP peer) |
| `port` | u16 | Client's UDP endpoint port |
| `ctxId` | u32 | Matching context ID |
| `serviceLabel` | u32 | Service label for the matching context |
| `serverId` | u16 | Server in the matching hierarchy |
| `worldId` | u16 | World in the matching hierarchy |
| `lobbyId` | u16 | Lobby in the matching hierarchy |
| `roomId` | u64 | Current room (0 if not in a room) |
| `myMemberId` | u16 | Member ID within the current room |
| `isRoomOwner` | bool | Whether this client owns the current room |
| `maxSlots` | u16 | Maximum room capacity |
| `roomFlags` | u32 | Room flags/settings |
| `enabledHandlersMask` | u8 | Bitmask of which callback handlers are active |
| `initialized` | bool | Whether RegisterHandlers has been called |

**Handler types** (7 total, registered via `RegisterHandlers`):

| Index | Handler | Description |
|---|---|---|
| 0 | Context | Matching context events |
| 1 | Request | Room request completion callbacks |
| 2 | Signaling | Peer signaling events |
| 3 | RoomEvent | Room state change events |
| 4 | LobbyEvent | Lobby events |
| 5 | RoomMessage | Room chat/data messages |
| 6 | LobbyMessage | Lobby messages |

Each handler stores a `callbackAddr` (u64) and `callbackArg` (u64) on the client side.

---

### Room

Stored in `MatchingSharedState::rooms`, keyed by `roomId`.

| Field | Type | Description |
|---|---|---|
| `roomId` | u64 | Unique room identifier (monotonically increasing) |
| `maxSlots` | u16 | Maximum number of members |
| `ownerNpid` | string | NP ID of the room owner |
| `serverId` | u16 | Server in the matching hierarchy |
| `worldId` | u16 | World in the matching hierarchy |
| `lobbyId` | u16 | Lobby in the matching hierarchy |
| `flags` | u32 | Room flags/settings |
| `teamId` | u16 | Team/group identifier |
| `groupConfigCount` | u16 | Number of team groups (typically 1) |
| `signalingType` | u8 | Signaling mode |
| `signalingFlag` | u8 | Signaling flags |
| `signalingMainMember` | u16 | Main signaling member |
| `roomPasswordPresent` | u8 | Whether a password is set |
| `joinGroupLabelPresent` | u8 | Whether a group label is set |
| `members` | map | Member ID → `RoomMember` |
| `binAttrsInternal` | list | Room-level binary attributes (synced to all members) |
| `externalSearchIntAttrs` | list | Searchable integer attributes (attrId + value pairs) |
| `externalSearchBinAttrs` | list | Searchable binary attributes |
| `externalBinAttrs` | list | Non-searchable external binary attributes |

**Room attribute types:**

| Type | Visibility | Purpose |
|---|---|---|
| Internal binary | All room members | Synchronize game state within the room |
| Search integer | Room list queries | Filterable integer metadata (e.g. game mode, map) |
| Search binary | Room list queries | Filterable binary metadata |
| External binary | Room list queries | Non-filterable binary metadata |

---

### RoomMember

| Field | Type | Description |
|---|---|---|
| `memberId` | u16 | Unique ID within the room |
| `npid` | string | Player's NP ID |
| `addr` | string | Client's UDP endpoint address |
| `port` | u16 | Client's UDP endpoint port |
| `binAttrsInternal` | list | Per-member binary attributes |

---

### MatchingSharedState (global)

Thread-safe shared state protected by `QReadWriteLock`. Lock ordering: `roomsLock` before `clientsLock`.

| Field | Type | Description |
|---|---|---|
| `rooms` | map | roomId → Room |
| `nextRoomId` | atomic u64 | Monotonically increasing room ID generator |
| `udpExt` | map | npid → (ip, port) — external UDP endpoints discovered by STUN |
| `signalingPairs` | map | (me_npid, peer_npid) → (peer_ip, peer_port) — P2P handshake tracking |
| `activationIntents` | map | (initiator_ip_u32, ctx_tag) → (initiator_npid, peer_npid) — NpSignaling activation |

---

## Commands

All matchmaking commands require a prior successful login. String fields are null-terminated UTF-8. Multi-byte integers are little-endian.

---

### RegisterHandlers (12)

Initialize the matchmaking subsystem for this session. Must be called before any room operations.

**Request payload:**
```
addr\0             — Client's UDP address (empty → use TCP peer address)
port      (u16 LE) — Client's UDP port
ctxId     (u32 LE) — Matching context ID
serviceLabel (u32 LE) — Service label

handlerCount (u8)  — Number of handlers (up to 7)
  for each handler:
    enabled      (u8)  — 1 = active, 0 = inactive
    callbackAddr (u64 LE) — Client-side callback address
    callbackArg  (u64 LE) — Client-side callback argument
```

**Reply payload:** error byte only (`NoError`).

Sets `initialized = true` on the session. The handler bitmask determines which notification types the client will receive.

---

### CreateRoom (13)

Create a new room with the caller as owner and sole member.

**Request payload:**
```
reqId              (u32 LE) — Client request ID
maxSlots           (u16 LE) — Maximum room capacity
teamId             (u16 LE) — Team/group identifier
worldId            (u16 LE) — World in hierarchy
lobbyId            (u16 LE) — Lobby in hierarchy
flags              (u32 LE) — Room flags
groupConfigCount   (u16 LE) — Number of team groups

allowedUserCount   (u16 LE) — (reserved, skipped)
blockedUserCount   (u16 LE) — (reserved, skipped)

— Internal binary attributes —
internalBinAttrCount (u16 LE)
  for each:
    attrId  (u16 LE)
    dataLen (u16 LE)
    data    (dataLen bytes)

— Searchable integer attributes —
extSearchIntAttrCount (u16 LE)
  for each:
    attrId (u16 LE)
    value  (u32 LE)

— Searchable binary attributes —
extSearchBinAttrCount (u16 LE)
  for each:
    attrId  (u16 LE)
    dataLen (u16 LE)
    data    (dataLen bytes)

— External binary attributes —
extBinAttrCount (u16 LE)
  for each:
    attrId  (u16 LE)
    dataLen (u16 LE)
    data    (dataLen bytes)

— Member binary attributes (for the creator) —
memberBinAttrCount (u16 LE)
  for each:
    attrId  (u16 LE)
    dataLen (u16 LE)
    data    (dataLen bytes)

joinGroupLabelPresent (u8)
roomPasswordPresent   (u8)
signalingType         (u8)
signalingFlag         (u8)
signalingMainMember   (u16 LE)
```

**Reply payload on success:**
```
roomId           (u64 LE) — Newly allocated room ID
serverId         (u16 LE) — Server in hierarchy
worldId          (u16 LE) — World in hierarchy
lobbyId          (u16 LE) — Lobby in hierarchy
memberId         (u16 LE) — Creator's member ID (always 1)
maxSlots         (u16 LE) — Room capacity
flags            (u32 LE) — Room flags
curMemberNum     (u16 LE) — Current member count (always 1)
...              full room data blob (internal view)
```

**Notifications pushed:**
- `RequestEvent` (to self, if Request handler enabled) — request code `0x0101` (CreateJoinRoom) with embedded room data

**State update:** session `roomId`, `myMemberId = 1`, `isRoomOwner = true`.

---

### JoinRoom (14)

Join an existing room.

**Request payload:**
```
roomId             (u64 LE) — Room to join
reqId              (u32 LE) — Client request ID
teamId             (u16 LE) — Team identifier
joinFlags          (u32 LE) — Join flags
blockedUserCount   (u16 LE) — (reserved, skipped)

— Member binary attributes (for the joiner) —
joinMemberBinAttrCount (u16 LE)
  for each:
    attrId  (u16 LE)
    dataLen (u16 LE)
    data    (dataLen bytes)

roomPasswordPresent   (u8)
joinGroupLabelPresent (u8)
```

**Reply payload on success:**
```
roomId           (u64 LE) — Joined room ID
memberId         (u16 LE) — Joiner's assigned member ID
maxSlots         (u16 LE) — Room capacity
flags            (u32 LE) — Room flags
curMemberNum     (u16 LE) — Current member count (after join)
...              full room data blob (internal view)
```

**Validation:** room must exist and not be full. The joining member must not already be in the room.

**Notifications pushed (in order):**

1. **MemberJoined** (to existing members) — joiner's npid, memberId, addr, port, and member binary attributes
2. **SignalingHelper** (bidirectional, if Signaling handler enabled) — for each existing member, sends them the joiner's info and sends joiner their info
3. **RoomDataInternalUpdated** (to joiner, if RoomEvent handler enabled) — current room's binary attributes
4. **SignalingEvent** (delayed 2 seconds, to all members) — `0x5102` ESTABLISHED event for each new peer pairing

**State update:** session `roomId`, `myMemberId`, `isRoomOwner = false`.

---

### LeaveRoom (15)

Leave the current room.

**Request payload:**
```
roomId (u64 LE) — Room to leave
reqId  (u32 LE) — Client request ID
```

**Reply payload:** error byte only (`NoError`).

**Leave logic (also triggered on disconnect):**

1. Remove self from room members
2. If room is now empty, destroy it
3. If leaving member was the owner, transfer ownership to the first remaining member
4. Clear all signaling pairs and activation intents related to the leaving member

**Notifications pushed:**
- **MemberLeft** (to remaining members) — leaver's roomId, memberId, npid
- **RequestEvent** (to self) — request code `0x0103` (LeaveRoom) with roomId

**State cleanup:** session `roomId = 0`, `myMemberId = 0`, `isRoomOwner = false`.

---

### GetRoomList (16)

Retrieve all rooms on the server.

**Request payload:** none.

**Reply payload:**
```
roomCount (u32 LE) — Number of rooms
  for each room:
    ...  full room data external (searchable attributes view)
```

Returns all rooms from the global rooms map with their external/searchable attributes.

---

### RequestSignalingInfos (17)

Request the UDP endpoint information for a specific peer in a shared room.

**Request payload:**
```
targetNpid\0 — NP ID of the target peer
```

**Reply payload on success:**
```
targetNpid\0          — Target's NP ID
targetIp   (4 bytes)  — Target's external IP (network order)
targetPort (u16 LE)   — Target's external UDP port
targetMemberId (u16 LE) — Target's member ID in the shared room
```

**Endpoint resolution order:**
1. Check `udpExt` map (populated by STUN ping)
2. Fallback: search all rooms for a matching member's addr/port

**Notifications pushed:**
- **SignalingHelper** (to target) — requester's npid, memberId, addr, port

---

### SignalingEstablished (18)

Notify the server that a P2P connection to a peer has been established (TCP-side logging).

**Request payload:**
```
targetNpid\0        — NP ID of the connected peer
connId     (u32 LE) — Connection ID
```

**Reply payload:** error byte only (`NoError`).

This command is informational — the server logs the event but takes no state-changing action.

---

### ActivationConfirm (19)

Confirm NpSignaling activation on the TCP channel after the UDP handshake completes.

**Request payload:**
```
meId\0                  — Own NP ID
initiatorIpStr\0        — Initiator's IP address as a string
ctxTag        (u32 LE)  — Context tag from the STUN HandleActivationIntent
```

**Reply payload:** error byte only (`NoError`).

The server looks up the activation intent (stored by the STUN server's `HandleActivationIntent` command) using the initiator IP and context tag. If found, it sends an `NpSignalingEvent` notification to the initiator confirming the connection is activated.

---

### SetRoomDataInternal (20)

Update the room's internal binary attributes and flags. Typically called by the room owner.

**Request payload:**
```
reqId       (u32 LE) — Client request ID
roomId      (u64 LE) — Target room
flagFilter  (u32 LE) — Bitmask of which flag bits to modify
flagAttr    (u32 LE) — New flag bit values (applied via filter)

binAttrCount (u16 LE) — Number of binary attributes to update
  for each:
    attrId  (u16 LE)
    dataLen (u16 LE)
    data    (dataLen bytes)

hasPasswdMask (u8)     — Whether a password slot mask follows
  if hasPasswdMask:
    passwdSlotMask (u64 LE)
```

**Reply payload:** error byte only (`NoError`).

**Flag update formula:** `room.flags = (room.flags & ~flagFilter) | (flagAttr & flagFilter)`

Binary attributes are updated or inserted by `attrId`.

**Notifications pushed:**
- **RequestEvent** (to self) — request code `0x0109` (SetRoomDataInternal)
- **RoomDataInternalUpdated** (broadcast to all room members except sender)

---

### SetRoomDataExternal (21)

Update the room's searchable and external attributes. Used for room discovery metadata.

**Request payload:**
```
reqId       (u32 LE) — Client request ID
roomId      (u64 LE) — Target room

intAttrCount (u16 LE) — Searchable integer attributes
  for each:
    attrId (u16 LE)
    value  (u32 LE)

searchBinAttrCount (u16 LE) — Searchable binary attributes
  for each:
    attrId  (u16 LE)
    dataLen (u16 LE)
    data    (dataLen bytes)

extBinAttrCount (u16 LE) — External (non-searchable) binary attributes
  for each:
    attrId  (u16 LE)
    dataLen (u16 LE)
    data    (dataLen bytes)
```

**Reply payload:** error byte only (`NoError`).

**Notifications pushed:**
- **RequestEvent** (to self) — request code `0x0004` (SetRoomDataExternal)

---

## Notifications

Matchmaking notifications are server-initiated `PacketType::Notification` packets. They are pushed asynchronously and require no reply.

| Value | Name | Description |
|---|---|---|
| `9` | `RequestEvent` | Room request completed |
| `10` | `MemberJoined` | A new member joined the room |
| `11` | `MemberLeft` | A member left the room |
| `12` | `SignalingHelper` | Peer address exchange for P2P discovery |
| `13` | `SignalingEvent` | NpMatching2-layer signaling event |
| `14` | `NpSignalingEvent` | NpSignaling-layer activation event |
| `15` | `RoomDataInternalUpdated` | Room internal binary attributes changed |

---

### RequestEvent (9)

Pushed to the requesting client after a room operation completes. Wraps the operation result in a notification so the client's Request handler callback fires.

**Payload:**
```
requestCode (u16 LE) — Operation that completed
responseBlob        — Command-specific response data (room state)
```

**Request codes:**

| Code | Operation |
|---|---|
| `0x0101` | CreateJoinRoom (CreateRoom or JoinRoom) |
| `0x0103` | LeaveRoom |
| `0x0109` | SetRoomDataInternal |
| `0x0004` | SetRoomDataExternal |

---

### MemberJoined (10)

Pushed to all existing room members when a new member joins.

**Payload:**
```
roomId          (u64 LE)
joinerMemberId  (u16 LE)
joinerNpid\0
joinerAddr\0             — Joiner's UDP address
joinerPort      (u16 LE) — Joiner's UDP port
binAttrCount    (u16 LE) — Joiner's member binary attributes
  for each:
    attrId  (u16 LE)
    dataLen (u16 LE)
    data    (dataLen bytes)
```

---

### MemberLeft (11)

Pushed to remaining room members when a member leaves or disconnects.

**Payload:**
```
roomId          (u64 LE)
leaverMemberId  (u16 LE)
leaverNpid\0
```

---

### SignalingHelper (12)

Exchanged bidirectionally between peers for P2P address discovery. Sent during JoinRoom and RequestSignalingInfos.

**Payload:**
```
peerNpid\0
peerMemberId (u16 LE)
peerAddr\0             — Peer's UDP address
peerPort     (u16 LE)  — Peer's UDP port
```

---

### SignalingEvent (13)

NpMatching2-layer signaling completion. Sent on a 2-second delay after a JoinRoom to give peers time to establish their UDP connection.

**Payload:**
```
eventCode   (u32 LE) — Always 0x5102 (ESTABLISHED)
roomId      (u64 LE)
peerMemberId (u16 LE) — Member ID of the peer
connId      (u16 LE)  — Connection ID (same as peerMemberId)
```

---

### NpSignalingEvent (14)

NpSignaling-layer activation confirmation. Sent after `ActivationConfirm` resolves a STUN activation intent.

**Payload:**
```
eventCode (u32 LE) — Always 1 (connection activated)
connId    (u32 LE) — Connection identifier
```

---

### RoomDataInternalUpdated (15)

Broadcast to all room members (except sender) when `SetRoomDataInternal` modifies the room's binary attributes.

**Payload:**
```
roomId (u64 LE)
...    full room internal binary attributes blob
```

---

## Disconnect Cleanup

When a client disconnects (TCP connection closed), the server automatically:

1. Calls `DoLeaveRoom` if the client is in a room — notifying remaining members via `MemberLeft`
2. Transfers room ownership if the disconnecting client was the owner
3. Destroys the room if it becomes empty
4. Purges stale signaling pairs and activation intents associated with the client

---

## Thread Safety

- All shared matchmaking state is protected by `QReadWriteLock`
- Room IDs are allocated via `atomic fetch_add` to avoid lock contention
- Lock ordering is always `roomsLock` before `clientsLock` to prevent deadlocks
- Each `ClientSession` runs in its own `QThread`
