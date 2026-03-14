# ShadNet Wire Protocol Reference

Complete specification of the binary protocol spoken between clients and the shadnet server. All multi-byte integers are **little-endian**. All strings are **null-terminated UTF-8**.


## Constants

| Constant | Value | Description |
|---|---|---|
| `HEADER_SIZE` | `15` | Fixed size of every packet header in bytes |
| `PROTOCOL_VERSION` | `1` | Version sent in the ServerInfo handshake |
| `MAX_PACKET_SIZE` | `0x800000` (8 MiB) | Maximum allowed total packet size |
| `COMMUNICATION_ID_SIZE` | `12` | Length of a `ComId` string (e.g. `ABCD12345_01`) |

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

| Value | Name | Direction | Description |
|---|---|---|---|
| `0` | `Request` | Client → Server | Client sends a command |
| `1` | `Reply` | Server → Client | Server responds to a Request |
| `2` | `Notification` | Server → Client | Server-initiated push (not used in auth-only mode) |
| `3` | `ServerInfo` | Server → Client | Version handshake, sent on connect |

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

Server-initiated push packet. The server sends these without a corresponding client request; no reply is expected or sent.

```
[0]     0x02                PacketType::Notification
[1-2]   type    (u16 LE)    NotificationType
[3-6]   size    (u32 LE)    15 + payload length
[7-14]  0x00 × 8            packetId = 0 (unused)
[15+]   payload             Type-specific, see Notifications
```

The client must read and discard any notification type it does not recognise. Notifications may arrive at any time after a successful login — including while waiting for a command reply.

### ServerInfo

Sent by the server immediately upon connection, before any command is exchanged. No reply is sent by the client.

```
[0]     0x03                PacketType::ServerInfo
[1-2]   0x00 0x00           command = 0 (unused)
[3-6]   0x13 0x00 0x00 0x00 size = 19 (15 header + 4 payload)
[7-14]  0x00 × 8            packetId = 0
[15-18] 0x1E 0x00 0x00 0x00 PROTOCOL_VERSION = e.g 1 (u32 LE)
```

If the received version does not equal to `PROTOCOL_VERSION` the client must close the connection immediately.

---

## Commands

String notation below uses `str\0` to mean a null-terminated UTF-8 string. An empty string is represented as a single `0x00` byte.

---

### Login (0)

Authenticate an existing account. On success the session transitions to authenticated state.

**Request payload:**
```
npid\0       — NP ID (username)
password\0   — Password in plaintext (hashed server-side)
token\0      — Email validation token; send empty string if EmailValidated = false
```

**Reply payload on success (`NoError`):**
```
onlineName\0         — Display name
avatarUrl\0          — Avatar URL (may be empty string)
userId    (u64 LE)   — Server-assigned user ID

— Friends list —
count     (u32 LE)   — Number of mutual friends
  for each friend:
    npid\0           — Friend's NP ID
    online (u8)      — 1 = currently online, 0 = offline
    presence         — Fixed 19-byte presence blob (see below)

— Pending outgoing requests —
count     (u32 LE)   — Number of requests you have sent but not yet accepted
  for each:
    npid\0           — Their NP ID

— Pending incoming requests —
count     (u32 LE)   — Number of requests sent to you but not yet accepted
  for each:
    npid\0           — Their NP ID

— Blocked users —
count     (u32 LE)   — Number of users you have blocked
  for each:
    npid\0           — Their NP ID
```

All four list sections are always present, even when their count is zero. The client reads them unconditionally and will fail to parse the reply if any section is missing.

**Presence blob layout (19 bytes, always written — empty when no presence data):**
```
comId     (12 bytes) — Communication ID; all-zero when offline
title\0              — Game title string; bare \0 when empty
status\0             — Status string; bare \0 when empty
comment\0            — Comment string; bare \0 when empty
dataLen  (u32 LE)    — Length of following data blob; 0 when empty
data     (dataLen)   — Arbitrary presence data
```

**Reply payload on failure:** error byte only, no additional data.

**Error codes:**

| Error | Cause |
|---|---|
| `LoginInvalidPassword` | NP ID not found, or password does not match |
| `LoginInvalidToken` | `EmailValidated = true` and token is empty or wrong |
| `LoginAlreadyLoggedIn` | Another session for this `userId` is already connected |
| `LoginError` | Account exists but is banned |
| `Malformed` | Payload could not be parsed |

---

### Terminate (1)

Request a clean disconnect. The server sends the reply then immediately closes the socket.

**Request payload:** none (empty)

**Reply payload:** error byte only (`NoError`)

Allowed both before and after login. Sending any further data after the reply is undefined behaviour — the server has already closed its end.

---

### Create (2)

Register a new account. The server sends the reply then **always** closes the connection, regardless of success or failure.

**Request payload:**
```
npid\0        — NP ID; 1–16 chars, alphanumeric + underscore + hyphen, must start with a letter
password\0    — Password
onlineName\0  — Display name; same validation rules as npid
avatarUrl\0   — Avatar URL; may be an empty string (bare \0)
email\0       — Valid email address
```

**Reply payload:** error byte only, no additional data.

**Post-reply behaviour:** the server closes the connection. The client does not need to send `Terminate`.

If `EmailValidated = true` in `shadnet.cfg`, the server emails the validation token to the supplied address. The token is never sent over the wire.

**Error codes:**

| Error | Cause |
|---|---|
| `NoError` | Account created; check email for token if validation is enabled |
| `InvalidInput` | NP ID or online name failed format validation, or email address is malformed |
| `CreationExistingUsername` | NP ID already registered (case-insensitive) |
| `CreationExistingEmail` | Email address already in use |
| `CreationBannedEmailProvider` | Email domain is on the server's banned domain list |
| `CreationError` | Database insert failed (see server log for details) |
| `Malformed` | Payload could not be parsed |

---

## Error Codes

Complete table of all `ErrorType` values. The error byte is always at offset 15 of every Reply packet.

| Value | Name | Description |
|---|---|---|
| `0` | `NoError` | Success |
| `1` | `Malformed` | Packet payload could not be parsed (missing or truncated fields) |
| `2` | `Invalid` | Command not valid in current state |
| `3` | `InvalidInput` | A field failed format or content validation |
| `4` | `TooSoon` | Rate limit — the same operation was performed too recently |
| `5` | `LoginError` | Generic login failure (account banned, or internal error) |
| `6` | `LoginAlreadyLoggedIn` | A session for this account is already active |
| `7` | `LoginInvalidUsername` | *(unused by auth server — folded into `LoginInvalidPassword`)* |
| `8` | `LoginInvalidPassword` | NP ID not found, or password hash mismatch |
| `9` | `LoginInvalidToken` | Email validation token missing or incorrect |
| `10` | `CreationError` | Database insert failed during account creation |
| `11` | `CreationExistingUsername` | NP ID already registered |
| `12` | `CreationBannedEmailProvider` | Email domain is banned |
| `13` | `CreationExistingEmail` | Email address already registered |
| `23` | `Unauthorized` | Command requires authentication, or insufficient privileges |
| `24` | `DbFail` | Database query failed |
| `25` | `EmailFail` | Failed to send email |
| `26` | `NotFound` | Requested resource does not exist |
| `33` | `Unsupported` | Command recognised but not implemented |

---

### Delete (3)

Permanently delete an account. Requires login.

**Request payload:**
```
npid\0       — NP ID of the account to delete
password\0   — Password of that account (re-verified before deletion)
```

**Reply payload:** error byte only.

The password is always verified against the target account, regardless of who is making the request. A user may only delete their own account; a session with `admin = true` may delete any account.

After a successful delete the server removes the target user from the active clients map (if they are currently logged in) and closes the connection of the requesting session, since the session is now invalid.

**Error codes:**

| Error | Cause |
|---|---|
| `NoError` | Account deleted; server closes connection |
| `LoginInvalidPassword` | Password verification failed, or NP ID not found |
| `Unauthorized` | Attempting to delete a different account without admin rights |
| `DbFail` | Database error during deletion |
| `Malformed` | Payload could not be parsed |

---

### AddFriend (8)

Send a friend request to another user, or accept a pending incoming request. Requires login.

**Request payload:**
```
targetNpid\0   — NP ID of the user to add
```

**Reply payload:** error byte only.

The server checks the current relationship state and acts accordingly:

- **No existing relationship, or only the other side has sent a request before** — sets the `Friend` flag on the caller's side. A `FriendQuery` notification is pushed to the target.
- **The target already has a pending outgoing request to the caller** — sets `Friend=1` on both sides, completing a mutual friendship. A `FriendNew` notification is sent to both parties.
- **Either side has `Blocked` set** — rejected with `Blocked`.
- **Caller's `Friend` flag is already set** — rejected with `AlreadyFriend` (covers both pending and confirmed states).

Sending to yourself returns `InvalidInput`.

**Error codes:**

| Error | Cause |
|---|---|
| `NoError` | Request sent, or friendship formed |
| `NotFound` | Target NP ID does not exist |
| `AlreadyFriend` | Request already sent, or already friends |
| `Blocked` | A block exists on either side |
| `InvalidInput` | Target NP ID is the caller's own NP ID |
| `DbFail` | Database error |
| `Malformed` | Payload could not be parsed |

---

### RemoveFriend (9)

Remove a friend or cancel a pending outgoing request. Requires login.

**Request payload:**
```
targetNpid\0   — NP ID of the friend to remove
```

**Reply payload:** error byte only.

Clears the `Friend` flag on both sides. If no flags remain on either side after clearing, the friendship row is deleted from the database. A `FriendLost` notification is sent to both parties.

Sending to yourself returns `InvalidInput`. Sending when no `Friend` flag exists on either side returns `NotFound`.

**Error codes:**

| Error | Cause |
|---|---|
| `NoError` | Friend removed |
| `NotFound` | No friend relationship exists with the target |
| `InvalidInput` | Target NP ID is the caller's own NP ID |
| `DbFail` | Database error |
| `Malformed` | Payload could not be parsed |

---

### AddBlock (10)

Block a user. Requires login.

**Request payload:**
```
targetNpid\0   — NP ID of the user to block
```

**Reply payload:** error byte only.

Sets `Blocked=1` on the caller's side and clears the `Friend` flag on both sides. If a mutual friendship existed at the time of the block, both parties receive a `FriendLost` notification.

After this command the target user cannot send the caller a friend request — `AddFriend` returns `Blocked` when either side has the flag set.

Sending to yourself returns `InvalidInput`.

**Error codes:**

| Error | Cause |
|---|---|
| `NoError` | User blocked |
| `NotFound` | Target NP ID does not exist |
| `InvalidInput` | Target NP ID is the caller's own NP ID |
| `DbFail` | Database error |
| `Malformed` | Payload could not be parsed |

---

### RemoveBlock (11)

Unblock a previously blocked user. Requires login.

**Request payload:**
```
targetNpid\0   — NP ID of the user to unblock
```

**Reply payload:** error byte only.

Clears `Blocked` on the caller's side. If no flags remain on either side after clearing, the relationship row is deleted from the database. Unblocking does **not** restore a friendship — both parties must go through `AddFriend` again if desired.

**Error codes:**

| Error | Cause |
|---|---|
| `NoError` | User unblocked |
| `NotFound` | Target NP ID does not exist, or was not blocked |
| `InvalidInput` | Target NP ID is the caller's own NP ID |
| `DbFail` | Database error |
| `Malformed` | Payload could not be parsed |

---

## Notifications

Notifications are server-initiated `PacketType::Notification` (type `0x02`) packets. They are pushed asynchronously at any time after a successful login. No reply is sent by the client.

The `type` field in the header carries the `NotificationType` value. The `packetId` field is always `0`.

**NotificationType values:**

| Value | Name | Description |
|---|---|---|
| `5` | `FriendQuery` | Someone sent you a friend request |
| `6` | `FriendNew` | A mutual friendship has been formed |
| `7` | `FriendLost` | Someone removed you or was blocked |
| `8` | `FriendStatus` | A friend came online or went offline |

---

### FriendQuery (5)

Pushed to the **target** when another user calls `AddFriend` and no mutual friendship is formed yet.

**Payload:**
```
fromNpid\0   — NP ID of the user who sent the request
```

---

### FriendNew (6)

Pushed to **both** users when `AddFriend` completes a mutual friendship (i.e. the caller accepts a pending incoming request). Each side receives the other's NP ID.

**Payload:**
```
online (u8)    — 1 = the new friend is currently online, 0 = offline
npid\0         — NP ID of the new friend (from the recipient's perspective)
```

---

### FriendLost (7)

Pushed to **both** users when `RemoveFriend` severs a relationship, or when `AddBlock` severs a mutual friendship. Each side receives the other's NP ID.

**Payload:**
```
npid\0   — NP ID of the user who is now gone (from the recipient's perspective)
```

---

### FriendStatus (8)

Pushed to all **online friends** of a user when that user logs in or disconnects.

**Payload:**
```
online    (u8)      — 1 = user came online, 0 = user went offline
timestamp (u64 LE)  — Event time in microseconds since the Common Era epoch (CE, 0001-01-01)
npid\0              — NP ID of the user whose status changed
```

The CE epoch is `Unix epoch + 62,135,596,800 seconds`. Convert to Unix time by subtracting `62135596800000000` microseconds.

Complete table of all `ErrorType` values. The error byte is always at offset 15 of every Reply packet.

| Value | Name | Description |
|---|---|---|
| `0` | `NoError` | Success |
| `1` | `Malformed` | Packet payload could not be parsed (missing or truncated fields) |
| `2` | `Invalid` | Command not valid in current state |
| `3` | `InvalidInput` | A field failed format or content validation |
| `4` | `TooSoon` | Rate limit — the same operation was performed too recently |
| `5` | `LoginError` | Generic login failure (account banned, or internal error) |
| `6` | `LoginAlreadyLoggedIn` | A session for this account is already active |
| `7` | `LoginInvalidUsername` | *(unused — folded into `LoginInvalidPassword`)* |
| `8` | `LoginInvalidPassword` | NP ID not found, or password hash mismatch |
| `9` | `LoginInvalidToken` | Email validation token missing or incorrect |
| `10` | `CreationError` | Database insert failed during account creation |
| `11` | `CreationExistingUsername` | NP ID already registered |
| `12` | `CreationBannedEmailProvider` | Email domain is banned |
| `13` | `CreationExistingEmail` | Email address already registered |
| `23` | `Unauthorized` | Command requires authentication, or insufficient privileges |
| `24` | `DbFail` | Database query failed |
| `25` | `EmailFail` | Failed to send email |
| `26` | `NotFound` | Requested resource does not exist |
| `27` | `Blocked` | Operation blocked because a block exists on one or both sides |
| `28` | `AlreadyFriend` | Friend request already sent, or already friends |
| `33` | `Unsupported` | Command recognised but not implemented |

---

## Encoding Reference

### Integers

All integers are stored little-endian with no padding.

| C++ type | Size | Example value | Wire bytes |
|---|---|---|---|
| `u8` | 1 | `30` | `1E` |
| `u16` | 2 | `256` | `00 01` |
| `u32` | 4 | `19` | `13 00 00 00` |
| `u64` | 8 | `1` | `01 00 00 00 00 00 00 00` |

### Strings

Strings are encoded as raw UTF-8 bytes followed by a single `0x00` terminator. There is no length prefix.

```
"george"  →  67 65 6F 72 67 65 00
""        →  00
```

### Packet ID assignment

The client assigns a monotonically increasing `packetId` starting from any non-zero value. The server echoes it verbatim in the Reply. The client uses this to correlate replies to requests. In a non-pipelined session (one outstanding request at a time) the value only needs to be unique per connection lifetime.

### Annotated Login request example

```
01                         ← packetId = 1 (shown below in full)
Byte stream:

00                         ← PacketType::Request
00 00                      ← CommandType::Login (0)
1F 00 00 00                ← size = 31 bytes total
01 00 00 00 00 00 00 00    ← packetId = 1
67 65 6F 72 67 65 00       ← npid    = "george\0"
73 65 63 72 65 74 00       ← password = "secret\0"
00                         ← token   = "\0" (empty — no email validation)
```

### Annotated Login reply example (success, no friends)

```
01                         ← PacketType::Reply
00 00                      ← CommandType::Login (0)
35 00 00 00                ← size = 53 bytes total
01 00 00 00 00 00 00 00    ← packetId = 1 (echoed)
00                         ← ErrorType::NoError
47 65 6F 72 67 65 4D 00    ← onlineName = "GeorgeM\0"
00                         ← avatarUrl  = "\0" (empty)
01 00 00 00 00 00 00 00    ← userId     = 1 (u64 LE)
00 00 00 00                ← friends count    = 0
00 00 00 00                ← req_sent count   = 0
00 00 00 00                ← req_recv count   = 0
00 00 00 00                ← blocked count    = 0
```

### Annotated Login reply example (one online friend)

```
01                         ← PacketType::Reply
00 00                      ← CommandType::Login (0)
...                        ← size updated accordingly
01 00 00 00 00 00 00 00    ← packetId = 1
00                         ← NoError
47 65 6F 72 67 65 4D 00    ← onlineName = "GeorgeM\0"
00                         ← avatarUrl = "" (empty)
01 00 00 00 00 00 00 00    ← userId = 1
01 00 00 00                ← friends count = 1
  41 6C 69 63 65 00        ←   npid = "Alice\0"
  01                       ←   online = 1 (online)
  00 00 00 00 00 00 00     ←   comId bytes 0-6 (empty)
  00 00 00 00 00           ←   comId bytes 7-11 (empty, with _ 0 0)
  00                       ←   title\0
  00                       ←   status\0
  00                       ←   comment\0
  00 00 00 00              ←   data length = 0
00 00 00 00                ← req_sent count  = 0
00 00 00 00                ← req_recv count  = 0
00 00 00 00                ← blocked count   = 0
```