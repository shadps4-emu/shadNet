# RPCN Auth — Wire Protocol Reference

Complete specification of the binary protocol spoken between clients and the shadnet server. All multi-byte integers are **little-endian**. All strings are **null-terminated UTF-8**.


## Constants

| Constant | Value | Description |
|---|---|---|
| `HEADER_SIZE` | `15` | Fixed size of every packet header in bytes |
| `PROTOCOL_VERSION` | `30` | Version sent in the ServerInfo handshake |
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

### Annotated Login reply example (success)

```
01                         ← PacketType::Reply
00 00                      ← CommandType::Login (0)
35 00 00 00                ← size = 53 bytes total
01 00 00 00 00 00 00 00    ← packetId = 1 (echoed)
00                         ← ErrorType::NoError
47 65 6F 72 67 65 4D 00    ← onlineName = "GeorgeM\0"
00                         ← avatarUrl  = "\0" (empty)
01 00 00 00 00 00 00 00    ← userId     = 1 (u64 LE)
00 00 00 00                ← friends    = 0 (u32 LE)
00 00 00 00                ← req_sent   = 0
00 00 00 00                ← req_recv   = 0
00 00 00 00                ← blocked    = 0
```