# Matchmaking (NpMatching2)

Room-based matchmaking. Clients start a matching context, query the world layout, then create or join rooms and synchronize room state via binary attributes. All matchmaking state is held in memory — rooms are not persisted across server restarts.

---

## Overview

Matchmaking follows the NpMatching2 model. A client establishes a matching context (`ContextStart`), discovers the world layout (`GetWorldInfoList`), then creates or joins rooms. Every room event (a member joining, leaving, being kicked, or room data changing) is delivered to room members through a single unified `RoomEvent` notification that maps directly onto the client's `OrbisNpMatching2RoomEventCallback`.

The server is the source of truth: it holds the full room and member state and ships the complete dataset to clients. The emulator keeps a local cache from those payloads.

---

## Scoping by title

Matchmaking data is scoped by a **matching key**, resolved from the title id the client reports at login (`LoginRequest.title_id`, the CUSA serial). At login the server computes:

```
matchingKey = titleGroups.value(titleId, titleId)
```

If the title id belongs to a configured group (see `worlds.cfg`), the group name is used as the key; otherwise the title id is its own key. Titles sharing a key share one room pool and world list — this lets regional/re-release SKUs of the same game match together. All room/world maps are keyed by `(matchingKey, ...)`.

---

## Wire Format

Matching commands and notifications use Protocol Buffers (proto3). Message definitions are in `shadnet.proto`.

| Direction | Layout |
|---|---|
| Request | `u32-LE size` + serialized proto bytes |
| Reply | `ErrorType (u8)` + `u32-LE size` + serialized proto bytes |
| Notification | `u32-LE size` + serialized proto bytes |

Commands with no meaningful reply body still return a proto message (e.g. `LeaveRoomReply { room_id }`). The error byte is always present first in every reply.

---

## Data Structures

### MatchingSessionState (per-session)

Each authenticated `ClientSession` holds a `MatchingSessionState`.

| Field | Type | Description |
|---|---|---|
| `ctxId` | u32 | Matching context ID (set by `ContextStart`) |
| `titleId` | string | Title id reported at login |
| `matchingKey` | string | Resolved scoping key (group or title id) |
| `serverId` | u16 | Server in the matching hierarchy |
| `worldId` | u16 | World in the matching hierarchy |
| `lobbyId` | u16 | Lobby in the matching hierarchy |
| `roomId` | u64 | Current room (0 if not in a room) |
| `myMemberId` | u16 | Member ID within the current room |
| `isRoomOwner` | bool | Whether this client owns the current room |
| `maxSlots` | u16 | Maximum room capacity |
| `roomFlags` | u32 | Room flags/settings |
| `initialized` | bool | Whether a context is active (`ContextStart` called) |

---

### Room

Stored in `MatchingSharedState::rooms`, keyed by `(matchingKey, roomId)`.

| Field | Type | Description |
|---|---|---|
| `roomId` | u64 | Unique room identifier (monotonically increasing) |
| `maxSlot` | u16 | Maximum number of members |
| `ownerMemberId` | u16 | Member ID of the room owner |
| `serverId` | u16 | Server in the matching hierarchy |
| `worldId` | u16 | World in the matching hierarchy |
| `lobbyId` | u16 | Lobby in the matching hierarchy |
| `flagAttr` | u32 | Room flags/settings |
| `members` | map | Member ID → `RoomMember` |
| `groups` | list | Team groups (`RoomGroup`) |
| `internalBinAttr` | slots | Room-level internal binary attributes (synced to members) |
| `searchIntAttr` / `searchBinAttr` | slots | Searchable attributes (room-list filtering) |
| `externalBinAttr` | slots | Non-searchable external binary attributes |
| `ownerSuccession` | list | Ordered fallback owners |

**Room attribute types:**

| Type | Visibility | Purpose |
|---|---|---|
| Internal binary | All room members | Synchronize game state within the room |
| Search integer/binary | Room list queries | Filterable metadata (e.g. game mode, map) |
| External binary | Room list queries | Non-filterable metadata |

---

### RoomMember

| Field | Type | Description |
|---|---|---|
| `memberId` | u16 | Unique ID within the room |
| `userId` | i64 | Server account ID |
| `npid` | string | Player's NP ID |
| `addr` / `port` | string / u16 | UDP endpoint (from STUN `udpExt`) |
| `joinDate` | u64 | Join timestamp (usec) |
| `flagAttr` | u32 | Member flags (owner bit, etc.) |
| `teamId` | u8 | Team identifier |
| `groupId` | u8 | Room group the member belongs to |
| `natType` | u8 | NAT type |
| `memberBinAttr` | slot | Per-member internal binary attribute |

The full member record is what the server packs into every room-member event (see `MatchingRoomMemberData`).

---

### MatchingSharedState (global)

Thread-safe shared state protected by `QReadWriteLock`. Lock ordering: `roomsLock` before `clientsLock`.

| Field | Type | Description |
|---|---|---|
| `rooms` | map | (matchingKey, roomId) → Room |
| `worldRooms` | map | (matchingKey, worldId) → [roomId] |
| `lobbyRooms` | map | (matchingKey, lobbyId) → [roomId] |
| `nextRoomId` | atomic u64 | Monotonically increasing room ID generator |
| `worldConfigs` | map | matchingKey → [WorldConfig] (from `worlds.cfg`) |
| `titleGroups` | map | titleId → group name (from `worlds.cfg`) |
| `udpExt` | map | npid → (ip, port) — external UDP endpoints discovered by STUN |

---

## World configuration (`worlds.cfg`)

A two-section INI file, loaded at startup into `titleGroups` and `worldConfigs`. Absent file → `GetWorldInfoList` returns a single default world.

```ini
[groups]
# titleId = group   (titleId as the client reports it: CUSAxxxxx, no dash)
CUSA00207 = bloodborne
CUSA00900 = bloodborne

[worlds]
# group | world_id | server_id | lobbies_num | max_lobby_members
bloodborne | 1 | 1 | 0 | 0
```

`[groups]` maps title ids to a shared matching key. `[worlds]` declares the worlds for a key. A title id not listed in `[groups]` keys by its own title id; its worlds (if any) are listed under that title id as the key.

---

## Commands

All matchmaking commands require a prior successful login. Matching command IDs: 12–23.

---

### ContextStart (12)

Establish the matching context for this session. Sent by the emulator when the game starts a context.

**Request:** `ContextStartRequest { ctx_id }`

**Reply:** `ErrorType(u8)` only.

Stores `ctxId` on the session and marks it initialized. The reply drives the emulator's `CONTEXT_EVENT_STARTED` callback.

---

### ContextStop (18)

Tear down the matching context.

**Request:** `ContextStopRequest { ctx_id }`

**Reply:** `ErrorType(u8)` only.

Clears the session's `ctxId` / initialized flag. The reply drives the emulator's `CONTEXT_EVENT_STOPPED` callback.

---

### CreateRoom (13)

Create a new room with the caller as owner and sole member.

**Request:** `CreateRoomRequest` proto

| Field | Type | Description |
|---|---|---|
| `req_id` | uint32 | Client request ID |
| `max_slots` | uint32 | Maximum room capacity |
| `team_id` | uint32 | Team/group identifier |
| `world_id` | uint32 | World in hierarchy |
| `lobby_id` | uint32 | Lobby in hierarchy |
| `flags` | uint32 | Room flags |
| `group_config_count` | uint32 | Number of team groups |
| `external_search_int_attrs` | repeated `MatchingIntAttr` | Searchable integer attributes |
| `external_search_bin_attrs` | repeated `MatchingBinAttr` | Searchable binary attributes |
| `external_bin_attrs` | repeated `MatchingBinAttr` | External non-searchable binary attributes |
| `member_bin_attrs` | repeated `MatchingBinAttr` | Creator's member binary attributes |
| `sig_type` / `sig_flag` / `sig_main_member` | uint32 | Signaling parameters carried through to room data |

**Reply:** `ErrorType(u8)` + `CreateRoomReply` proto (room_id, server/world/lobby, member_id, max_slots, flags, cur_members, and `details` = `CreateJoinRoomResponse` with full room data + member list).

**State update:** session `roomId`, `myMemberId = 1`, `isRoomOwner = true`. The room is indexed under `(matchingKey, roomId)` and its world.

---

### JoinRoom (14)

Join an existing room.

**Request:** `JoinRoomRequest` proto (room_id, req_id, team_id, join_flags, member_bin_attrs, password/group-label presence flags).

**Reply:** `ErrorType(u8)` + `JoinRoomReply` proto (room_id, member_id, max_slots, flags, cur_members, `details` = `CreateJoinRoomResponse`).

**Validation:** room must exist and not be full; the joiner must not already be a member.

**Notifications pushed:**
- **RoomEvent** `MEMBER_JOINED (0x1101)` to existing members — carries the full joining member.
- **RoomEvent** `UPDATED_ROOM_DATA_INTERNAL (0x1106)` to the joiner — the room's current internal binary attributes.

**State update:** session `roomId`, `myMemberId`, `isRoomOwner = false`.

---

### LeaveRoom (15)

Leave the current room.

**Request:** `LeaveRoomRequest { room_id, req_id }`

**Reply:** `ErrorType(u8)` + `LeaveRoomReply { room_id }`

**Leave logic (also triggered on disconnect):**
1. Remove self from room members.
2. If the room is now empty, destroy it (and remove it from the world/lobby indices).
3. If the leaver was the owner, transfer ownership via `ownerSuccession` (fallback: first remaining member). @TODO SCENPMATCHING2GRANTROOMOWNER

**Notifications pushed:**
- **RoomEvent** `MEMBER_LEFT (0x1102)` to remaining members, cause `LEAVE_ACTION`.

---

### GetRoomList (16)

Retrieve rooms for a world/lobby in this matching key, filtered by attributes.

**Request:** `GetRoomListRequest` (world_id, lobby_id, range filters, attribute filters).

**Reply:** `ErrorType(u8)` + `GetRoomListReply` (`rooms` = repeated `MatchingRoomDataExternal`, plus range_start/total/result). Candidates are taken from `worldRooms`/`lobbyRooms` for the caller's matching key and passed through the request filters.

---

### RequestSignalingInfos (17)

Look up a peer's UDP endpoint for P2P. On-demand only — no signaling state machine.

**Request:** `RequestSignalingInfosRequest { target_npid }`

**Reply:** `ErrorType(u8)` + `RequestSignalingInfosReply` (target_npid, target_ip, target_port, target_member_id).

**Endpoint resolution order:**
1. `udpExt` map (populated by STUN ping).
2. Fallback: search the caller's rooms for a member with that npid.

No notifications.

---

### SetRoomDataInternal (20)

Update the room's internal binary attributes and flags. Typically called by the owner.

**Request:** `SetRoomDataInternalRequest` (req_id, room_id, flag_filter, flag_attr, bin_attrs, optional passwd_slot_mask).

**Reply:** `ErrorType(u8)` + `SetRoomDataInternalReply { room_id }`

**Flag update:** `room.flagAttr = (room.flagAttr & ~flag_filter) | (flag_attr & flag_filter)` (the FULL bit is protected). Binary attributes are updated or inserted by slot.

**Notifications pushed:**
- **RoomEvent** `UPDATED_ROOM_DATA_INTERNAL (0x1106)` broadcast to all members except the sender.

---

### SetRoomDataExternal (21)

Update the room's searchable/external attributes (room-discovery metadata).

**Request:** `SetRoomDataExternalRequest` (req_id, room_id, search_int_attrs, search_bin_attrs, ext_bin_attrs).

**Reply:** `ErrorType(u8)` + `SetRoomDataExternalReply { room_id }`. No notifications.

---

### KickoutRoomMember (22)

Remove a member from the room. Owner only.

**Request:** `KickoutRoomMemberRequest` (room_id, req_id, target_member_id, block_kick_flag).

**Reply:** `ErrorType(u8)` + `KickoutRoomMemberReply { room_id }`

**Notifications pushed:**
- **RoomEvent** `KICKEDOUT (0x1103)` to the kicked member (cause `KICKOUT_ACTION`, error/status code).
- **RoomEvent** `MEMBER_LEFT (0x1102)` to remaining members (cause `KICKOUT_ACTION`).

---

### GetWorldInfoList (23)

Retrieve the world layout for a server id.

**Request:** `GetWorldInfoListRequest { server_id }`

**Reply:** `ErrorType(u8)` + `GetWorldInfoListReply { repeated MatchingWorld worlds }`

For the caller's matching key: if `worldConfigs` has configured worlds, those are returned (filtered by `server_id`); otherwise worlds are derived from active rooms, or a single default world. Each world carries live `rooms_num` / `room_members_num` counts computed from current rooms.

---

## Notifications

Matchmaking uses a **single** notification type. The server pushes `PacketType::Notification` packets; no reply is expected.

| Value | Name | Proto message | Description |
|---|---|---|---|
| `10` | `RoomEvent` | `NotifyRoomEvent` | Any room event (mapped to the room-event callback) |

### RoomEvent (10)

Every room event maps onto `OrbisNpMatching2RoomEventCallback(ctxId, roomId, event, data)`. The Orbis event id is carried in `event`; the data shape depends on it.

**Proto:** `NotifyRoomEvent`

| Field | Type | Description |
|---|---|---|
| `ctx_id` | uint32 | Owning matching context |
| `room_id` | uint64 | Room involved |
| `event` | uint32 | Orbis room event id (`0x11xx`) |
| `event_cause` | uint32 | `OrbisNpMatching2EventCause` |
| `error_code` | int32 | Result/status code (room-update events) |
| `member` | `MatchingRoomMemberData` | Full member (member events) |
| `bin_attrs` | repeated `MatchingBinAttr` | Updated attrs (room-data-updated) |
| `flags` | uint32 | Updated room flags (room-data-updated) |

**Event ids:**

| `event` | Meaning | Carries |
|---|---|---|
| `0x1101` | MEMBER_JOINED | full `member`, cause `SERVER_OPERATION` |
| `0x1102` | MEMBER_LEFT | full `member`, cause `LEAVE_ACTION` or `KICKOUT_ACTION` |
| `0x1103` | KICKEDOUT | `error_code`, cause `KICKOUT_ACTION` |
| `0x1106` | UPDATED_ROOM_DATA_INTERNAL | `flags` + `bin_attrs` |

`MatchingRoomMemberData` carries the **complete** member: npid, member_id, team_id, is_owner, group_id, nat_type, flag_attr, join_date, addr/port, and `bin_attrs_internal`. The emulator stores this in its local room cache and forwards it directly to the callback.

---

## Disconnect Cleanup

When a client disconnects (TCP closed), the server automatically:
1. Calls `DoLeaveRoom` if the client is in a room — notifying remaining members via `RoomEvent` `MEMBER_LEFT`.
2. Transfers room ownership if the disconnecting client was the owner.
3. Destroys the room if it becomes empty.

---

## Thread Safety

- All shared matchmaking state is protected by `QReadWriteLock`.
- Room IDs are allocated via `atomic fetch_add`.
- Lock ordering is always `roomsLock` before `clientsLock`.
- Each `ClientSession` runs in its own `QThread`. Notifications to other sessions are marshaled onto the target session's thread via `QMetaObject::invokeMethod`.
