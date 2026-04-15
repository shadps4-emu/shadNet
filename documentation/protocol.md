# ShadNet Wire Protocol Reference

Complete specification of the binary protocol spoken between clients and the shadnet server. All multi-byte integers are **little-endian**.

**Encoding note:** account and friend commands use **Protocol Buffers 3** (protobuf3) for all payloads. Score commands have always used protobuf3. The only commands that still use raw null-terminated strings are `Delete (3)`, `Terminate (1)`, and the inner fields of `ServerInfo`. See the [Encoding Reference](#encoding-reference) section for protobuf wire-format details.

---

## Constants

| Constant                | Value              | Description                                      |
| ----------------------- | ------------------ | ------------------------------------------------ |
| `HEADER_SIZE`           | `15`               | Fixed size of every packet header in bytes       |
| `PROTOCOL_VERSION`      | `1`                | Version sent in the ServerInfo handshake         |
| `MAX_PACKET_SIZE`       | `0x800000` (8 MiB) | Maximum allowed total packet size                |
| `COMMUNICATION_ID_SIZE` | `12`               | Length of a `ComId` string (e.g. `ABCD12345_01`) |

---

## Packet Layout

### Header

Every packet — regardless of type or direction — begins with this fixed 15-byte header.

```
Offset  Size  Type    Field       Description
──────  ────  ──────  ──────────  ────────────────────────────────────────────
0       1     u8      type        PacketType (see below)
1       2     u16 LE  command     CommandType (see Commands)
3       4     u32 LE  size        Total packet length including this header
7       8     u64 LE  packetId    Client-assigned request ID; echoed in Reply
```

**PacketType values:**

| Value | Name           | Direction       | Description                        |
| ----- | -------------- | --------------- | ---------------------------------- |
| `0`   | `Request`      | Client → Server | Client sends a command             |
| `1`   | `Reply`        | Server → Client | Server responds to a Request       |
| `2`   | `Notification` | Server → Client | Server-initiated push              |
| `3`   | `ServerInfo`   | Server → Client | Version handshake, sent on connect |

### Request

```
[0]     0x00                PacketType::Request
[1-2]   command (u16 LE)    CommandType
[3-6]   size    (u32 LE)    15 + payload length
[7-14]  packetId (u64 LE)   Client-chosen ID; must be unique per in-flight request
[15+]   payload             Command-specific, see Commands
```

### Reply

```
[0]     0x01                PacketType::Reply
[1-2]   command (u16 LE)    Echoed from the Request
[3-6]   size    (u32 LE)    15 + payload length
[7-14]  packetId (u64 LE)   Echoed from the Request
[15]    error   (u8)        ErrorType — 0x00 = NoError
[16+]   payload             Present only on success for commands that return data
```

The error byte at offset 15 is always present in a Reply, even when payload is otherwise empty.

### Notification

Server-initiated push packet. No reply is expected or sent.

```
[0]     0x02                PacketType::Notification
[1-2]   type    (u16 LE)    NotificationType
[3-6]   size    (u32 LE)    15 + payload length
[7-14]  0x00 × 8            packetId = 0 (unused)
[15+]   payload             u32 LE blob size + protobuf bytes (see Notifications)
```

The client must read and discard any notification type it does not recognise.

### ServerInfo

Sent by the server immediately upon connection, before any command is exchanged.

```
[0]     0x03                PacketType::ServerInfo
[1-2]   0x00 0x00           command = 0 (unused)
[3-6]   0x13 0x00 0x00 0x00 size = 19 (15 header + 4 payload)
[7-14]  0x00 × 8            packetId = 0
[15-18] 0x01 0x00 0x00 0x00 PROTOCOL_VERSION = 1 (u32 LE)
```

If the received version does not equal `PROTOCOL_VERSION` the client must close the connection immediately.

---

## Commands

### Protobuf payload convention

For all commands that use protobuf3, the request and reply payloads follow this framing:

```
size   (u32 LE)   — byte length of the protobuf message that follows
data   (bytes)    — serialised protobuf3 message
```

This is referred to below as a **proto blob**. The server reads it with `getRawData()` / `decodeProto<T>()` and writes replies with `appendProto()`. Fields at their proto3 default (0 / empty string / false) are omitted on the wire.

---

### Login (0)

Authenticate an existing account. On success the session transitions to authenticated state.

**Request payload:** proto blob of `LoginRequest`

```protobuf
message LoginRequest {
    string npid     = 1;   // NP ID (username)
    string password = 2;   // Password in plaintext (hashed server-side)
    string token    = 3;   // Email validation token; omit or leave empty if not required
}
```

**Reply payload on success (`NoError`):** proto blob of `LoginReply`

```protobuf
message FriendEntry {
    string npid     = 1;   // Friend's NP ID
    bool   online   = 2;   // true = currently online
    bytes  presence = 3;   // Reserved; always empty in current implementation
}

message LoginReply {
    string               avatar_url               = 1;  // Avatar URL (may be empty)
    uint64               user_id                  = 2;  // Server-assigned account ID
    repeated FriendEntry friends                  = 4;  // Mutual friends
    repeated string      friend_requests_sent     = 5;  // Pending outgoing requests
    repeated string      friend_requests_received = 6;  // Pending incoming requests
    repeated string      blocked                  = 7;  // Blocked users
}
```

> **Note:** field number 3 is intentionally absent from `LoginReply` — it was previously used for `online_name` which has been removed. The NP ID itself serves as the display name on PS4.

All four list fields are always present in the decoded message, even when empty. The client must not assume any list is non-empty before iterating.

**Reply payload on failure:** error byte only, no proto blob.

**Error codes:**

| Error                  | Cause                                                  |
| ---------------------- | ------------------------------------------------------ |
| `LoginInvalidPassword` | NP ID not found, or password does not match            |
| `LoginInvalidToken`    | `EmailValidated = true` and token is empty or wrong    |
| `LoginAlreadyLoggedIn` | Another session for this `userId` is already connected |
| `LoginError`           | Account exists but is banned                           |
| `Malformed`            | Proto blob could not be parsed                         |

---

### Terminate (1)

Request a clean disconnect. The server sends the reply then immediately closes the socket.

**Request payload:** none (empty)

**Reply payload:** error byte only (`NoError`)

Allowed both before and after login.

---

### Create (2)

Register a new account. The server always closes the connection after sending the reply.

**Request payload:** proto blob of `RegistrationRequest`

```protobuf
message RegistrationRequest {
    string npid       = 1;   // NP ID; 1–16 chars, alphanumeric + underscore + hyphen
    string password   = 2;   // Password
    string avatar_url = 3;   // Optional; server uses a default avatar if empty
    string email      = 4;   // Valid email address
    string secret_key = 5;   // Optional; must match RegistrationSecretKey in shadnet.cfg if set
}
```

**`secret_key` behaviour:**

- If `RegistrationSecretKey` is not set in `shadnet.cfg` (or is empty), the field is ignored — registration is open.
- If `RegistrationSecretKey` is set, the client must supply the matching value. A missing or wrong key returns `Unauthorized`.

The NP ID is used as the display name — there is no separate `online_name` field.

**Reply payload:** error byte only, no proto blob.

**Error codes:**

| Error                         | Cause                                                            |
| ----------------------------- | ---------------------------------------------------------------- |
| `NoError`                     | Account created                                                  |
| `Unauthorized`                | `secret_key` does not match the server's `RegistrationSecretKey` |
| `InvalidInput`                | NP ID failed format validation, or email address is malformed    |
| `CreationExistingUsername`    | NP ID already registered (case-insensitive)                      |
| `CreationExistingEmail`       | Email address already in use                                     |
| `CreationBannedEmailProvider` | Email domain is on the server's banned domain list               |
| `CreationError`               | Database insert failed (see server log for details)              |
| `Malformed`                   | Proto blob could not be parsed                                   |

---

### Delete (3)

Permanently delete an account. Requires login.

**Request payload:** raw null-terminated strings (not protobuf)

```
npid\0       — NP ID of the account to delete
password\0   — Password of that account (re-verified before deletion)
```

**Reply payload:** error byte only.

A user may only delete their own account; a session with `admin = true` may delete any account. After a successful delete the server closes the connection of the requesting session.

**Error codes:**

| Error                  | Cause                                                         |
| ---------------------- | ------------------------------------------------------------- |
| `NoError`              | Account deleted; server closes connection                     |
| `LoginInvalidPassword` | Password verification failed, or NP ID not found              |
| `Unauthorized`         | Attempting to delete a different account without admin rights |
| `DbFail`               | Database error during deletion                                |
| `Malformed`            | Payload could not be parsed                                   |

---

### AddFriend (8)

Send a friend request, or accept a pending incoming request. Requires login.

**Request payload:** proto blob of `FriendCommandRequest`

```protobuf
message FriendCommandRequest {
    string npid = 1;   // NP ID of the target user
}
```

**Reply payload:** error byte only.

Server behaviour:

- **No existing relationship** — sets `Friend` on the caller's side; sends `FriendQuery` to the target.
- **Target already sent a request to the caller** — sets `Friend=1` on both sides; sends `FriendNew` to both.
- **Either side has `Blocked`** — returns `Blocked`.
- **Caller's `Friend` flag already set** — returns `AlreadyFriend`.
- **Target is caller** — returns `InvalidInput`.

**Error codes:**

| Error           | Cause                                    |
| --------------- | ---------------------------------------- |
| `NoError`       | Request sent, or friendship formed       |
| `NotFound`      | Target NP ID does not exist              |
| `AlreadyFriend` | Request already sent, or already friends |
| `Blocked`       | A block exists on either side            |
| `InvalidInput`  | Target NP ID is the caller's own NP ID   |
| `DbFail`        | Database error                           |
| `Malformed`     | Proto blob could not be parsed           |

---

### RemoveFriend (9)

Remove a friend or cancel a pending outgoing request. Requires login.

**Request payload:** proto blob of `FriendCommandRequest` (same message as AddFriend)

**Reply payload:** error byte only.

Clears the `Friend` flag on both sides. A `FriendLost` notification is sent to both parties.

**Error codes:**

| Error          | Cause                                         |
| -------------- | --------------------------------------------- |
| `NoError`      | Friend removed                                |
| `NotFound`     | No friend relationship exists with the target |
| `InvalidInput` | Target NP ID is the caller's own NP ID        |
| `DbFail`       | Database error                                |
| `Malformed`    | Proto blob could not be parsed                |

---

### AddBlock (10)

Block a user. Requires login.

**Request payload:** proto blob of `FriendCommandRequest`

**Reply payload:** error byte only.

Sets `Blocked=1` on the caller's side and clears `Friend` on both sides. If a mutual friendship existed, both parties receive a `FriendLost` notification.

**Error codes:**

| Error          | Cause                                  |
| -------------- | -------------------------------------- |
| `NoError`      | User blocked                           |
| `NotFound`     | Target NP ID does not exist            |
| `InvalidInput` | Target NP ID is the caller's own NP ID |
| `DbFail`       | Database error                         |
| `Malformed`    | Proto blob could not be parsed         |

---

### RemoveBlock (11)

Unblock a previously blocked user. Requires login.

**Request payload:** proto blob of `FriendCommandRequest`

**Reply payload:** error byte only.

Clears `Blocked` on the caller's side. Unblocking does **not** restore a friendship — both parties must go through `AddFriend` again if desired.

**Error codes:**

| Error          | Cause                                           |
| -------------- | ----------------------------------------------- |
| `NoError`      | User unblocked                                  |
| `NotFound`     | Target NP ID does not exist, or was not blocked |
| `InvalidInput` | Target NP ID is the caller's own NP ID          |
| `DbFail`       | Database error                                  |
| `Malformed`    | Proto blob could not be parsed                  |

---

## NPScore Commands

All NPScore commands require login. Every request payload begins with a raw 12-byte **Communication ID** (`comId`) followed by a proto blob. The server reads the ComId with `getBytes(12)` and the protobuf blob with `getRawData()`.

Reply blobs that carry data are also `u32 LE` size + proto bytes.

---

### GetBoardInfos (30)

**Request payload:**

```
comId    (12 bytes)  — Communication ID
boardId  (u32 LE)    — Board number [raw u32, not protobuf]
```

**Reply payload on success:** proto blob of `BoardInfo`

```protobuf
message BoardInfo {
    uint32 rankLimit       = 1;
    uint32 updateMode      = 2;   // 0=NORMAL_UPDATE, 1=FORCE_UPDATE
    uint32 sortMode        = 3;   // 0=DESCENDING, 1=ASCENDING
    uint32 uploadNumLimit  = 4;
    uint32 uploadSizeLimit = 5;
}
```

**Error codes:** `NoError` | `NotFound` | `Malformed`

---

### RecordScore (31)

**Request payload:**

```
comId    (12 bytes)
```

proto blob of `RecordScoreRequest`

```protobuf
message RecordScoreRequest {
    uint32 boardId  = 1;
    int32  pcId     = 2;   // character/slot index; 0 for single-character games
    int64  score    = 3;
    string comment  = 4;   // optional
    bytes  data     = 5;   // optional inline game-info blob
}
```

**Reply payload on success:**

```
rank (u32 LE)   — 1-based position; rankLimit+1 = did not make the board
```

**Error codes:** `NoError` | `ScoreNotBest` | `DbFail` | `Malformed`

---

### RecordScoreData (32)

Attach a binary game-data blob to the most recently recorded score. Must be called immediately after a successful `RecordScore`.

**Request payload:**

```
comId    (12 bytes)
```

proto blob of `RecordScoreGameDataRequest`

```
size     (u32 LE)    — Length of raw game-data blob
data     (raw bytes)
```

```protobuf
message RecordScoreGameDataRequest {
    uint32 boardId = 1;
    int32  pcId    = 2;
    int64  score   = 3;   // must match the score from the preceding RecordScore
}
```

**Reply payload:** error byte only.

**Error codes:** `NoError` | `NotFound` | `ScoreInvalid` | `ScoreHasData` | `DbFail` | `Malformed`

---

### GetScoreData (33)

**Request payload:**

```
comId    (12 bytes)
```

proto blob of `GetScoreGameDataRequest`

```protobuf
message GetScoreGameDataRequest {
    uint32 boardId = 1;
    string npId    = 2;
    int32  pcId    = 3;
}
```

**Reply payload on success:**

```
size  (u32 LE)    — Length of raw data blob
data  (raw bytes)
```

**Error codes:** `NoError` | `NotFound` | `Malformed`

---

### GetScoreRange (34)

**Request payload:**

```
comId    (12 bytes)
```

proto blob of `GetScoreRangeRequest`

```protobuf
message GetScoreRangeRequest {
    uint32 boardId      = 1;
    uint32 startRank    = 2;   // 1-based
    uint32 numRanks     = 3;
    bool   withComment  = 4;
    bool   withGameInfo = 5;
}
```

**Reply payload on success:** proto blob of `GetScoreResponse`

```protobuf
message ScoreRankData {
    string npId        = 1;
    int32  pcId        = 2;
    uint32 rank        = 3;   // 1-based
    int64  score       = 4;
    bool   hasGameData = 5;
    uint64 recordDate  = 6;   // microseconds since CE epoch (0001-01-01)
}

message ScoreInfo {
    bytes data = 1;
}

message GetScoreResponse {
    repeated ScoreRankData rankArray    = 1;
    repeated string        commentArray = 2;   // only when withComment=true
    repeated ScoreInfo     infoArray    = 3;   // only when withGameInfo=true
    uint64                 lastSortDate = 4;
    uint32                 totalRecord  = 5;
}
```

**Error codes:** `NoError` | `Malformed`

---

### GetScoreFriends (35)

**Request payload:**

```
comId    (12 bytes)
```

proto blob of `GetScoreFriendsRequest`

```protobuf
message GetScoreFriendsRequest {
    uint32 boardId      = 1;
    bool   includeSelf  = 2;
    uint32 max          = 3;
    bool   withComment  = 4;
    bool   withGameInfo = 5;
}
```

**Reply payload:** same `GetScoreResponse` format as `GetScoreRange`.

**Error codes:** same as `GetScoreRange`.

---

### GetScoreNpid (36)

**Request payload:**

```
comId    (12 bytes)
```

proto blob of `GetScoreNpIdRequest`

```protobuf
message ScoreNpIdPcId {
    string npid = 1;
    int32  pcId = 2;
}

message GetScoreNpIdRequest {
    uint32                 boardId      = 1;
    repeated ScoreNpIdPcId npids        = 2;
    bool                   withComment  = 3;
    bool                   withGameInfo = 4;
}
```

**Reply payload:** same `GetScoreResponse` format. One entry per `ScoreNpIdPcId`, in request order.

**Error codes:** same as `GetScoreRange`.

---

## Error Codes

| Value | Name                          | Description                                                 |
| ----- | ----------------------------- | ----------------------------------------------------------- |
| `0`   | `NoError`                     | Success                                                     |
| `1`   | `Malformed`                   | Packet payload could not be parsed                          |
| `2`   | `Invalid`                     | Command not valid in current state                          |
| `3`   | `InvalidInput`                | A field failed format or content validation                 |
| `4`   | `TooSoon`                     | Rate limit                                                  |
| `5`   | `LoginError`                  | Generic login failure (account banned)                      |
| `6`   | `LoginAlreadyLoggedIn`        | A session for this account is already active                |
| `7`   | `LoginInvalidUsername`        | *(unused — folded into `LoginInvalidPassword`)*             |
| `8`   | `LoginInvalidPassword`        | NP ID not found, or password hash mismatch                  |
| `9`   | `LoginInvalidToken`           | Email validation token missing or incorrect                 |
| `10`  | `CreationError`               | Database insert failed during account creation              |
| `11`  | `CreationExistingUsername`    | NP ID already registered                                    |
| `12`  | `CreationBannedEmailProvider` | Email domain is banned                                      |
| `13`  | `CreationExistingEmail`       | Email address already registered                            |
| `23`  | `Unauthorized`                | Requires authentication, or `secret_key` mismatch on Create |
| `24`  | `DbFail`                      | Database query failed                                       |
| `25`  | `EmailFail`                   | Failed to send email                                        |
| `26`  | `NotFound`                    | Requested resource does not exist                           |
| `27`  | `Blocked`                     | A block exists on one or both sides                         |
| `28`  | `AlreadyFriend`               | Friend request already sent, or already friends             |
| `29`  | `ScoreNotBest`                | Score rejected — better score already recorded              |
| `30`  | `ScoreInvalid`                | Score value does not match stored entry                     |
| `31`  | `ScoreHasData`                | Score entry already has game data attached                  |
| `33`  | `Unsupported`                 | Command recognised but not implemented                      |

---

## Notifications

Notifications are `PacketType::Notification` (type `0x02`) packets pushed asynchronously after login. All notification payloads are **proto blobs** (u32 LE size + protobuf bytes) — the same framing as command payloads.

**NotificationType values:**

| Value | Name           | Description                          |
| ----- | -------------- | ------------------------------------ |
| `5`   | `FriendQuery`  | Someone sent you a friend request    |
| `6`   | `FriendNew`    | A mutual friendship has been formed  |
| `7`   | `FriendLost`   | Someone removed you or was blocked   |
| `8`   | `FriendStatus` | A friend came online or went offline |

---

### FriendQuery (5)

Pushed to the **target** when `AddFriend` does not yet form a mutual friendship.

```protobuf
message NotifyFriendQuery {
    string from_npid = 1;   // NP ID of the user who sent the request
}
```

---

### FriendNew (6)

Pushed to **both** users when `AddFriend` completes a mutual friendship.

```protobuf
message NotifyFriendNew {
    string npid   = 1;   // NP ID of the new friend (from recipient's perspective)
    bool   online = 2;   // true = new friend is currently online
}
```

---

### FriendLost (7)

Pushed to **both** users when `RemoveFriend` or `AddBlock` severs a relationship.

```protobuf
message NotifyFriendLost {
    string npid = 1;   // NP ID of the user who is now gone
}
```

---

### FriendStatus (8)

Pushed to all **online friends** when a user logs in or disconnects.

```protobuf
message NotifyFriendStatus {
    string npid      = 1;   // NP ID of the user whose status changed
    bool   online    = 2;   // true = came online, false = went offline
    uint64 timestamp = 3;   // nanoseconds since Unix epoch
}
```

---

## Encoding Reference

### Integers

All integers are stored little-endian with no padding.

| C++ type | Size | Example value | Wire bytes                |
| -------- | ---- | ------------- | ------------------------- |
| `u8`     | 1    | `30`          | `1E`                      |
| `u16`    | 2    | `256`         | `00 01`                   |
| `u32`    | 4    | `19`          | `13 00 00 00`             |
| `u64`    | 8    | `1`           | `01 00 00 00 00 00 00 00` |

### Protobuf blob framing

Every protobuf payload sent over this protocol is length-prefixed:

```
size   (u32 LE)   — byte length of the serialised protobuf message
data   (bytes)    — protobuf3 serialised bytes
```

The server writes this with `appendBlob(buf, serialised)` which calls `appendU32LE(size)` then `buf.append(data)`. The client reads it with `ExtractBlob(payload, pos)` which reads the u32 length then extracts that many bytes for `ParseFromString`.

### Packet ID assignment

The client assigns a monotonically increasing `packetId` starting from any non-zero value. The server echoes it verbatim in the Reply.

### Annotated Login request example

```
00                         ← PacketType::Request
00 00                      ← CommandType::Login (0)
28 00 00 00                ← size = 40 bytes total
01 00 00 00 00 00 00 00    ← packetId = 1

— proto blob —
10 00 00 00                ← blob size = 16 bytes (u32 LE)
0a 06 67 65 6f 72 67 65    ← field 1 (npid):     "george"
12 06 73 65 63 72 65 74    ← field 2 (password): "secret"
                           ← field 3 (token) omitted — empty string is proto3 default
```

### Annotated Login reply example (success, no friends)

```
01                         ← PacketType::Reply
00 00                      ← CommandType::Login (0)
24 00 00 00                ← size = 36 bytes total
01 00 00 00 00 00 00 00    ← packetId = 1 (echoed)
00                         ← ErrorType::NoError

— proto blob —
0b 00 00 00                ← blob size = 11 bytes (u32 LE)
0a 00                      ← field 1 (avatar_url): "" (empty — technically omitted but shown)
10 01                      ← field 2 (user_id):    1
                           ← fields 4-7 omitted — all empty lists are proto3 default
```

> Note: `onlineName` does not appear in the reply. The NP ID the client sent in the request is used as the display name.

### Annotated Create request example (with secret key)

```
00                         ← PacketType::Request
02 00                      ← CommandType::Create (2)
3c 00 00 00                ← size = 60 bytes total
01 00 00 00 00 00 00 00    ← packetId = 1

— proto blob —
25 00 00 00                ← blob size = 37 bytes (u32 LE)
0a 06 53 68 61 64 6f 77    ← field 1 (npid):       "Shadow"
12 04 31 32 33 34          ← field 2 (password):   "1234"
                           ← field 3 (avatar_url) omitted
22 12 73 68 61 64 6f 77    ← field 4 (email):      "shadow@test.local"
   40 74 65 73 74 2e 6c
   6f 63 61 6c
2a 09 4d 79 53 65 63 72    ← field 5 (secret_key): "MySecret"
   65 74
```
