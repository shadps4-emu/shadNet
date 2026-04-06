# Signaling (STUN Server & P2P Connection)

UDP-based signaling service for NAT traversal and peer-to-peer connection establishment. Runs alongside the main TCP server on a separate UDP port and coordinates with the TCP matchmaking layer to confirm P2P connections.

---

## Overview

The signaling system enables clients behind NAT to discover their external endpoints and establish direct P2P connections. It operates in two layers:

1. **UDP layer (STUN server)** — Handles NAT discovery, symmetric peer handshakes, and activation intent registration
2. **TCP layer (matchmaking commands)** — Exchanges peer addresses via `SignalingHelper` notifications and confirms activation via `ActivationConfirm`

The two layers work together to provide a complete signaling pipeline from room join through to confirmed P2P connectivity.

---

## Configuration

| Setting | Default | Description |
|---|---|---|
| `MatchingUdpPort` | `31314` | UDP port the STUN server listens on |

Set in `shadnet.cfg`. The STUN server binds to the same `Host` address as the TCP server.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        ShadNetServer                        │
│                                                             │
│  ┌──────────────────┐         ┌──────────────────────────┐  │
│  │   TCP Listener   │         │      StunServer (UDP)    │  │
│  │   port 31313     │         │      port 31314          │  │
│  │                  │         │                          │  │
│  │  ClientSession   │         │  HandleStunPing      (1) │  │
│  │  ├─ RegisterH.   │         │  HandleSignalingEst. (2) │  │
│  │  ├─ CreateRoom    │         │  HandleActivationI. (4) │  │
│  │  ├─ JoinRoom      │◄──────►│                          │  │
│  │  ├─ RequestSig.   │ shared │                          │  │
│  │  ├─ SignalingEst. │  state │                          │  │
│  │  └─ ActivationC. │         │                          │  │
│  └──────────────────┘         └──────────────────────────┘  │
│                                                             │
│  ┌────────────────────────────────────────────────────────┐ │
│  │              MatchingSharedState                        │ │
│  │  udpExt:            npid → (external_ip, external_port)│ │
│  │  signalingPairs:    (me, peer) → (peer_ip, peer_port)  │ │
│  │  activationIntents: (ip_u32, ctx_tag) → (me, peer)     │ │
│  └────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

---

## STUN Server Protocol

The STUN server communicates over raw UDP datagrams. Each datagram starts with a 1-byte command identifier. All IP addresses in payloads are 4 bytes in **network byte order** (big-endian). Ports are 2 bytes in **network byte order**. NP IDs are 16 bytes, null-padded.

---

### HandleStunPing (Command 0x01)

NAT traversal discovery. The client sends its NP ID and local IP; the server replies with the client's external IP and port as seen from the server's perspective.

**Request datagram (21 bytes):**
```
Offset  Size  Field         Description
──────  ────  ──────────    ────────────────────────────────────────
0       1     cmd           0x01
1       16    npid          NP ID, null-padded to 16 bytes
17      4     localIp       Client's local IP (network order)
```

**Reply datagram (6 bytes):**
```
Offset  Size  Field         Description
──────  ────  ──────────    ────────────────────────────────────────
0       4     externalIp    Client's external IP as seen by server (network order)
4       2     externalPort  Client's external port as seen by server (network order)
```

**Side effects:**
- Stores `npid → (externalIp, externalPort)` in `MatchingSharedState.udpExt`
- This endpoint is later used by `RequestSignalingInfos` (TCP command 17) to resolve peer addresses

**Purpose:** Allows the client to learn its public-facing UDP endpoint for hole-punching behind NAT.

---

### HandleSignalingEstablished (Command 0x02)

Symmetric P2P handshake. Both peers send this command naming each other. When the server has received the command from both sides, it notifies both peers simultaneously.

**Request datagram (33 bytes):**
```
Offset  Size  Field     Description
──────  ────  ────────  ────────────────────────────────────────
0       1     cmd       0x02
1       16    meId      Sender's NP ID, null-padded to 16 bytes
17      16    peerId    Target peer's NP ID, null-padded to 16 bytes
```

**Server logic:**

1. Store `(meId, peerId) → (senderIp, senderPort)` in `signalingPairs`
2. Check if reverse key `(peerId, meId)` already exists
   - **Not found:** no reply — wait for the other peer
   - **Found (symmetric match):** delete both entries and reply to both peers

**Reply datagram (17 bytes, sent to both peers on symmetric match):**
```
Offset  Size  Field     Description
──────  ────  ────────  ────────────────────────────────────────
0       1     cmd       0x03 (acknowledgment)
1       16    peerId    The OTHER peer's NP ID (so each side learns who connected)
```

**Purpose:** Ensures both peers have confirmed their P2P connection before notifying either side. Prevents one-sided connection states.

---

### HandleActivationIntent (Command 0x04)

Register an NpSignaling activation intent from the UDP side. The TCP-side `ActivationConfirm` command later resolves this to send the final activation notification.

**Request datagram (37 bytes):**
```
Offset  Size  Field     Description
──────  ────  ────────  ────────────────────────────────────────
0       1     cmd       0x04
1       16    meId      Sender's NP ID, null-padded to 16 bytes
17      16    peerId    Target peer's NP ID, null-padded to 16 bytes
33      4     ctxTag    Context tag (little-endian u32)
```

**Server logic:**

- Converts sender's IP to a network-order u32
- Stores `(senderIp_u32, ctxTag) → (meId, peerId)` in `activationIntents`
- No reply is sent — the TCP `ActivationConfirm` command completes the flow

**Purpose:** Bridges the UDP handshake to the TCP notification system. The context tag correlates the UDP intent with the TCP confirmation.

---

## Complete Signaling Flow

The full peer-to-peer connection flow from room join to confirmed connectivity:

```
  Client A (in room)                    Server                     Client B (joining)
  ──────────────────                    ──────                     ──────────────────

                                                            ◄───── JoinRoom (TCP cmd 14)
                                                                   Server adds B to room

  ◄──── MemberJoined (notif 10) ────                       ────── JoinRoom reply ──────►
  ◄──── SignalingHelper (notif 12) ──  exchange addrs  ──── SignalingHelper (notif 12) ─►
        (B's addr:port)                                           (A's addr:port)

  ── StunPing (UDP 0x01) ──────────►                       ◄───── StunPing (UDP 0x01) ──
  ◄── external IP:port reply ───────                       ────── external IP:port reply►

                    ┌─── 2 second delay ───┐
  ◄── SignalingEvent 0x5102 (notif 13) ──  ESTABLISHED  ── SignalingEvent 0x5102 (notif 13) ─►
        (B's memberId)                                           (A's memberId)

  ── SignalingEstablished (UDP 0x02) ─►                    ◄──── SignalingEstablished (UDP 0x02)
     meId=A, peerId=B                    symmetric match       meId=B, peerId=A

                                     both matched:
  ◄── UDP 0x03 ack (peerId=B) ──────                      ────── UDP 0x03 ack (peerId=A) ─►

  ── ActivationIntent (UDP 0x04) ───►  stores intent
                                        with ctx_tag
                                                            ◄──── ActivationConfirm (TCP cmd 19)
                                                                   resolves intent
  ◄── NpSignalingEvent (notif 14) ───  event=1 activated

  ── SignalingEstablished (TCP 18) ──►  (logging only)
```

### Step-by-step walkthrough

1. **Room join & address exchange** — When Client B joins the room, the server sends `SignalingHelper` notifications to both peers containing each other's UDP address and port.

2. **NAT discovery** — Both clients send a `StunPing` (UDP cmd 0x01) to the STUN server. The server records their external UDP endpoints and replies with their public IP:port.

3. **Matching-layer signaling** — After a 2-second delay (to allow UDP hole-punching), the server sends `SignalingEvent` (0x5102 ESTABLISHED) notifications to all members for each new peer pairing.

4. **P2P handshake** — Both clients send `HandleSignalingEstablished` (UDP cmd 0x02) naming each other. The server waits for both sides, then sends an acknowledgment (UDP cmd 0x03) to both simultaneously.

5. **Activation intent** — One peer registers an activation intent via UDP (cmd 0x04) with a context tag.

6. **Activation confirmation** — The other peer confirms via TCP (`ActivationConfirm`, cmd 19). The server resolves the intent and sends an `NpSignalingEvent` (event=1) to the initiator, completing the signaling pipeline.

7. **TCP logging** — The client sends `SignalingEstablished` (TCP cmd 18) to log the successful connection.

---

## Shared State

The STUN server and TCP matchmaking commands share state through `MatchingSharedState`. All access is synchronized via `QReadWriteLock`.

### udpExt

```
Key:   npid (QString)
Value: (externalIp, externalPort) — QPair<QString, u16>
Lock:  udpLock (QReadWriteLock)
```

Written by `HandleStunPing`. Read by `RequestSignalingInfos` (TCP) to resolve peer endpoints.

### signalingPairs

```
Key:   (meNpid, peerNpid) — QPair<QString, QString>
Value: (senderIp, senderPort) — QPair<QString, u16>
Lock:  signalingLock (QReadWriteLock)
```

Written by `HandleSignalingEstablished`. Entries exist only until a symmetric match is found (both directions present), at which point both are deleted. Stale entries are cleaned up on client disconnect.

### activationIntents

```
Key:   (initiatorIp_u32, ctxTag) — QPair<u32, u32>
Value: (initiatorNpid, peerNpid) — QPair<QString, QString>
Lock:  activationLock (QReadWriteLock)
```

Written by `HandleActivationIntent` (UDP). Read and consumed by `ActivationConfirm` (TCP). Stale entries are cleaned up on client disconnect.

---

## Disconnect Cleanup

When a client disconnects, the matchmaking cleanup routine purges signaling state:

- All `signalingPairs` entries where the disconnecting client is either `me` or `peer`
- All `activationIntents` entries where the disconnecting client is either the initiator or the peer
- The `udpExt` entry for the disconnecting client's NP ID is implicitly stale (will be overwritten on reconnect)

This prevents orphaned handshake state from accumulating when clients drop unexpectedly.
