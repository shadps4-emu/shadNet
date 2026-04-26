# Matchmaking (NpMatching2)

Room-based matchmaking system. Clients create or join rooms, exchange peer information for P2P connectivity, and synchronize room state via binary attributes. All matchmaking state is held in memory — rooms are not persisted across server restarts.

---

## Overview

Matchmaking follows the NpMatching2 model: clients register callback handlers, then create or join rooms. When a new member joins, the server automatically exchanges signaling information between all room members so they can establish peer-to-peer connections. Room owners can update room attributes that are either broadcast to members (internal) or made available for room discovery (external/searchable).

---

## Wire Format

All matching commands (12–22) and notifications (9–17) use Protocol Buffers (proto3). Proto message definitions are in `shadnet.proto`.

| Direction | Layout |
|---|---|
| Request | `u32-LE size` + serialized proto bytes |
| Reply | `ErrorType (u8)` + `u32-LE size` + serialized proto bytes |
| Notification | `u32-LE size` + serialized proto bytes |

Commands that have no meaningful reply body still return a proto message (e.g. `LeaveRoomReply { room_id }`). The error byte is always present first in every reply.

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

Each handler carries a `callback_addr` (u64) and `callback_arg` (u64) on the client side.

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

All matchmaking commands require a prior successful login.

---

### RegisterHandlers (12)

Initialize the matchmaking subsystem for this session. Must be called before any room operations.

**Request:** `RegisterHandlersRequest` proto

| Field | Type | Description |
|---|---|---|
| `addr` | string | Client's UDP address (empty → use TCP peer address) |
| `port` | uint32 | Client's UDP port |
| `ctx_id` | uint32 | Matching context ID |
| `service_label` | uint32 | Service label |
| `callbacks` | repeated `MatchingCallbackEntry` | Up to 7 handler entries |

`MatchingCallbackEntry` fields: `enabled` (bool), `callback_addr` (uint64), `callback_arg` (uint64).

**Reply:** `ErrorType(u8)` only — no proto body on success.

Sets `initialized = true` on the session. The handler bitmask determines which notification types the client will receive.

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
| `allowed_user_count` | uint32 | Reserved |
| `blocked_user_count` | uint32 | Reserved |
| `internal_bin_attr_count` | uint32 | Reserved (room's initial internal attr slots) |
| `external_search_int_attrs` | repeated `MatchingIntAttr` | Searchable integer attributes |
| `external_search_bin_attrs` | repeated `MatchingBinAttr` | Searchable binary attributes |
| `external_bin_attrs` | repeated `MatchingBinAttr` | External non-searchable binary attributes |
| `member_bin_attrs` | repeated `MatchingBinAttr` | Creator's member binary attributes |
| `join_group_label_present` | bool | Whether a join group label is provided |
| `room_password_present` | bool | Whether a room password is provided |
| `sig_type` | uint32 | Signaling mode |
| `sig_flag` | uint32 | Signaling flags |
| `sig_main_member` | uint32 | Main signaling member ID |

**Reply:** `ErrorType(u8)` + `CreateRoomReply` proto

| Field | Type | Description |
|---|---|---|
| `room_id` | uint64 | Newly allocated room ID |
| `server_id` | uint32 | Server in hierarchy |
| `world_id` | uint32 | World in hierarchy |
| `lobby_id` | uint32 | Lobby in hierarchy |
| `member_id` | uint32 | Creator's member ID (always 1) |
| `max_slots` | uint32 | Room capacity |
| `flags` | uint32 | Room flags |
| `cur_members` | uint32 | Current member count (always 1) |
| `details` | `CreateJoinRoomResponse` | Full room data (internal view) + member list |

**Notifications pushed:**
- `RequestEvent` (to self, if Request handler enabled) — `req_event = 0x0101` (CreateJoinRoom), `response_blob` carries serialized `CreateJoinRoomResponse`

**State update:** session `roomId`, `myMemberId = 1`, `isRoomOwner = true`.

---

### JoinRoom (14)

Join an existing room.

**Request:** `JoinRoomRequest` proto

| Field | Type | Description |
|---|---|---|
| `room_id` | uint64 | Room to join |
| `req_id` | uint32 | Client request ID |
| `team_id` | uint32 | Team identifier |
| `join_flags` | uint32 | Join flags |
| `blocked_user_count` | uint32 | Reserved |
| `member_bin_attrs` | repeated `MatchingBinAttr` | Joiner's member binary attributes |
| `room_password_present` | bool | Whether a room password is provided |
| `join_group_label_present` | bool | Whether a join group label is provided |

**Reply:** `ErrorType(u8)` + `JoinRoomReply` proto

| Field | Type | Description |
|---|---|---|
| `room_id` | uint64 | Joined room ID |
| `member_id` | uint32 | Joiner's assigned member ID |
| `max_slots` | uint32 | Room capacity |
| `flags` | uint32 | Room flags |
| `cur_members` | uint32 | Current member count (after join) |
| `details` | `CreateJoinRoomResponse` | Full room data (internal view) + member list |

**Validation:** room must exist and not be full. The joining member must not already be in the room.

**Notifications pushed (in order):**

1. **MemberJoined** (to existing members) — joiner's npid, memberId, addr, port, and member binary attributes
2. **SignalingHelper** (bidirectional, if Signaling handler enabled) — for each existing member, sends them the joiner's info and sends joiner their info
3. **RoomDataInternalUpdated** (to joiner, if RoomEvent handler enabled) — current room's binary attributes
4. **SignalingEvent** (delayed 2 seconds, to all members) — `event_type = 0x5102` (ESTABLISHED) for each new peer pairing

**State update:** session `roomId`, `myMemberId`, `isRoomOwner = false`.

---

### LeaveRoom (15)

Leave the current room.

**Request:** `LeaveRoomRequest` proto

| Field | Type | Description |
|---|---|---|
| `room_id` | uint64 | Room to leave |
| `req_id` | uint32 | Client request ID |

**Reply:** `ErrorType(u8)` + `LeaveRoomReply` proto

| Field | Type | Description |
|---|---|---|
| `room_id` | uint64 | Room that was left |

**Leave logic (also triggered on disconnect):**

1. Remove self from room members
2. If room is now empty, destroy it
3. If leaving member was the owner, transfer ownership to the first remaining member
4. Clear all signaling pairs and activation intents related to the leaving member

**Notifications pushed:**
- **MemberLeft** (to remaining members) — leaver's roomId, memberId, npid
- **RequestEvent** (to self) — `req_event = 0x0103` (LeaveRoom), `response_blob` carries serialized `LeaveRoomReply`

**State cleanup:** session `roomId = 0`, `myMemberId = 0`, `isRoomOwner = false`.

---

### GetRoomList (16)

Retrieve all rooms on the server.

**Request:** no proto body.

**Reply:** `ErrorType(u8)` + `GetRoomListReply` proto

| Field | Type | Description |
|---|---|---|
| `rooms` | repeated `MatchingRoomDataExternal` | All rooms with their external/searchable attributes |

Returns all rooms from the global rooms map.

---

### RequestSignalingInfos (17)

Request the UDP endpoint information for a specific peer in a shared room.

**Request:** `RequestSignalingInfosRequest` proto

| Field | Type | Description |
|---|---|---|
| `target_npid` | string | NP ID of the target peer |

**Reply:** `ErrorType(u8)` + `RequestSignalingInfosReply` proto

| Field | Type | Description |
|---|---|---|
| `target_npid` | string | Target's NP ID |
| `target_ip` | string | Target's external IP address |
| `target_port` | uint32 | Target's external UDP port |
| `target_member_id` | uint32 | Target's member ID in the shared room |

**Endpoint resolution order:**
1. Check `udpExt` map (populated by STUN ping)
2. Fallback: search all rooms for a matching member's addr/port

**Notifications pushed:**
- **SignalingHelper** (to target) — requester's npid, memberId, addr, port

---

### SignalingEstablished (18)

Notify the server that a P2P connection to a peer has been established (TCP-side logging).

**Request:** `SignalingEstablishedRequest` proto

| Field | Type | Description |
|---|---|---|
| `target_npid` | string | NP ID of the connected peer |
| `conn_id` | uint32 | Connection ID |

**Reply:** `ErrorType(u8)` only — no proto body.

Informational — the server logs the event but takes no state-changing action.

---

### ActivationConfirm (19)

Confirm NpSignaling activation on the TCP channel after the UDP handshake completes.

**Request:** `ActivationConfirmRequest` proto

| Field | Type | Description |
|---|---|---|
| `me_id` | string | Own NP ID |
| `initiator_ip` | string | Initiator's IP address as a string |
| `ctx_tag` | uint32 | Context tag from the STUN `HandleActivationIntent` command |

**Reply:** `ErrorType(u8)` only — no proto body.

The server looks up the activation intent (stored by the STUN server's `HandleActivationIntent` command) using the initiator IP and context tag. If found, it sends an `NpSignalingEvent` notification to the initiator confirming the connection is activated.

---

### SetRoomDataInternal (20)

Update the room's internal binary attributes and flags. Typically called by the room owner.

**Request:** `SetRoomDataInternalRequest` proto

| Field | Type | Description |
|---|---|---|
| `req_id` | uint32 | Client request ID |
| `room_id` | uint64 | Target room |
| `flag_filter` | uint32 | Bitmask of which flag bits to modify |
| `flag_attr` | uint32 | New flag bit values (applied via filter) |
| `bin_attrs` | repeated `MatchingBinAttr` | Binary attributes to update |
| `has_passwd_mask` | bool | Whether `passwd_slot_mask` is present |
| `passwd_slot_mask` | uint64 | Password slot mask (only meaningful if `has_passwd_mask`) |

**Reply:** `ErrorType(u8)` + `SetRoomDataInternalReply` proto

| Field | Type | Description |
|---|---|---|
| `room_id` | uint64 | Room that was updated |

**Flag update formula:** `room.flags = (room.flags & ~flag_filter) | (flag_attr & flag_filter)`

Binary attributes are updated or inserted by `attr_id`.

**Notifications pushed:**
- **RequestEvent** (to self) — `req_event = 0x0109` (SetRoomDataInternal), `response_blob` carries serialized `SetRoomDataInternalReply`
- **RoomDataInternalUpdated** (broadcast to all room members except sender)

---

### SetRoomDataExternal (21)

Update the room's searchable and external attributes. Used for room discovery metadata.

**Request:** `SetRoomDataExternalRequest` proto

| Field | Type | Description |
|---|---|---|
| `req_id` | uint32 | Client request ID |
| `room_id` | uint64 | Target room |
| `search_int_attrs` | repeated `MatchingIntAttr` | Searchable integer attributes |
| `search_bin_attrs` | repeated `MatchingBinAttr` | Searchable binary attributes |
| `ext_bin_attrs` | repeated `MatchingBinAttr` | External non-searchable binary attributes |

**Reply:** `ErrorType(u8)` + `SetRoomDataExternalReply` proto

| Field | Type | Description |
|---|---|---|
| `room_id` | uint64 | Room that was updated |

**Notifications pushed:**
- **RequestEvent** (to self) — `req_event = 0x0004` (SetRoomDataExternal), `response_blob` carries serialized `SetRoomDataExternalReply`

---

### KickoutRoomMember (22)

Remove a member from the room. Only the room owner may call this.

**Request:** `KickoutRoomMemberRequest` proto

| Field | Type | Description |
|---|---|---|
| `room_id` | uint64 | Target room |
| `req_id` | uint32 | Client request ID |
| `target_member_id` | uint32 | Member ID of the player to kick |
| `block_kick_flag` | uint32 | Flags controlling the kick behavior |

**Reply:** `ErrorType(u8)` + `KickoutRoomMemberReply` proto

| Field | Type | Description |
|---|---|---|
| `room_id` | uint64 | Room from which the member was removed |

**Notifications pushed:**
- **KickedOut** (to the kicked member) — room_id, status_code, guard_value
- **MemberLeft** (to remaining members) — kicked member's roomId, memberId, npid
- **RequestEvent** (to self/owner) — `req_event` for KickoutRoomMember, `response_blob` carries serialized `KickoutRoomMemberReply`

---

## Notifications

Matchmaking notifications are server-initiated `PacketType::Notification` packets. They are pushed asynchronously and require no reply. Each notification payload is `u32-LE size` + serialized proto bytes.

| Value | Name | Proto message | Description |
|---|---|---|---|
| `9` | `RequestEvent` | `NotifyRequestEvent` | Room request completed |
| `10` | `MemberJoined` | `NotifyMemberJoined` | A new member joined the room |
| `11` | `MemberLeft` | `NotifyMemberLeft` | A member left the room |
| `12` | `SignalingHelper` | `NotifySignalingHelper` | Peer address exchange for P2P discovery |
| `13` | `SignalingEvent` | `NotifySignalingEvent` | NpMatching2-layer signaling event |
| `14` | `NpSignalingEvent` | `NotifyNpSignalingEvent` | NpSignaling-layer activation event |
| `15` | `RoomDataInternalUpdated` | `NotifyRoomDataInternalUpdated` | Room internal binary attributes changed |
| `16` | `KickedOut` | `NotifyKickedOut` | This client was kicked from the room |
| `17` | `SignalingMute` | `NotifySignalingMute` | Peer mute event |

---

### RequestEvent (9)

Pushed to the requesting client after a room operation completes. Wraps the operation result so the client's Request handler callback fires.

**Proto:** `NotifyRequestEvent`

| Field | Type | Description |
|---|---|---|
| `ctx_id` | uint32 | Matching context ID |
| `server_id` | uint32 | Server in hierarchy |
| `world_id` | uint32 | World in hierarchy |
| `lobby_id` | uint32 | Lobby in hierarchy |
| `req_event` | uint32 | Operation code that completed (see table below) |
| `req_id` | uint32 | Client request ID echoed from the command |
| `error_code` | uint32 | Result code |
| `room_id` | uint64 | Room involved |
| `member_id` | uint32 | Caller's member ID |
| `max_slots` | uint32 | Room capacity |
| `flags` | uint32 | Room flags |
| `is_owner` | bool | Whether caller is the room owner |
| `response_blob` | bytes | Serialized inner proto — type inferred from `req_event` |

**`req_event` codes and their `response_blob` types:**

| `req_event` | Operation | `response_blob` proto |
|---|---|---|
| `0x0101` | CreateJoinRoom | `CreateJoinRoomResponse` |
| `0x0103` | LeaveRoom | `LeaveRoomReply` |
| `0x0109` | SetRoomDataInternal | `SetRoomDataInternalReply` |
| `0x0004` | SetRoomDataExternal | `SetRoomDataExternalReply` |

---

### MemberJoined (10)

Pushed to all existing room members when a new member joins.

**Proto:** `NotifyMemberJoined`

| Field | Type | Description |
|---|---|---|
| `room_id` | uint64 | Room the member joined |
| `member_id` | uint32 | Joiner's member ID |
| `npid` | string | Joiner's NP ID |
| `addr` | string | Joiner's UDP address |
| `port` | uint32 | Joiner's UDP port |
| `bin_attrs` | repeated `MatchingBinAttr` | Joiner's member binary attributes |

---

### MemberLeft (11)

Pushed to remaining room members when a member leaves or disconnects.

**Proto:** `NotifyMemberLeft`

| Field | Type | Description |
|---|---|---|
| `room_id` | uint64 | Room the member left |
| `member_id` | uint32 | Leaving member's ID |
| `npid` | string | Leaving member's NP ID |

---

### SignalingHelper (12)

Exchanged bidirectionally between peers for P2P address discovery. Sent during JoinRoom and RequestSignalingInfos.

**Proto:** `NotifySignalingHelper`

| Field | Type | Description |
|---|---|---|
| `npid` | string | Peer's NP ID |
| `member_id` | uint32 | Peer's member ID in the room |
| `addr` | string | Peer's UDP address |
| `port` | uint32 | Peer's UDP port |

---

### SignalingEvent (13)

NpMatching2-layer signaling completion. Sent on a 2-second delay after a JoinRoom to give peers time to establish their UDP connection.

**Proto:** `NotifySignalingEvent`

| Field | Type | Description |
|---|---|---|
| `event_type` | uint32 | Always `0x5102` (ESTABLISHED) |
| `room_id` | uint64 | Room |
| `member_id` | uint32 | Member ID of the peer |
| `conn_id` | uint32 | Connection ID (same as `member_id`) |

---

### NpSignalingEvent (14)

NpSignaling-layer activation confirmation. Sent after `ActivationConfirm` resolves a STUN activation intent.

**Proto:** `NotifyNpSignalingEvent`

| Field | Type | Description |
|---|---|---|
| `event` | uint32 | Always `1` (connection activated) |
| `npid` | string | NP ID of the peer whose activation was confirmed |

---

### RoomDataInternalUpdated (15)

Broadcast to all room members (except sender) when `SetRoomDataInternal` modifies the room's binary attributes.

**Proto:** `NotifyRoomDataInternalUpdated`

| Field | Type | Description |
|---|---|---|
| `room_id` | uint64 | Room that was updated |
| `flags` | uint32 | Updated room flags |
| `bin_attrs` | repeated `MatchingBinAttr` | Updated internal binary attributes |

---

### KickedOut (16)

Sent to a member that has been removed from the room by the owner.

**Proto:** `NotifyKickedOut`

| Field | Type | Description |
|---|---|---|
| `room_id` | uint64 | Room from which the client was kicked |
| `status_code` | int32 | Status code indicating reason for kick |
| `guard_value` | uint32 | Guard value from the kickout request |

---

### SignalingMute (17)

Peer mute event.

**Proto:** `NotifySignalingMute`

| Field | Type | Description |
|---|---|---|
| `npid` | string | NP ID of the peer to mute |

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
