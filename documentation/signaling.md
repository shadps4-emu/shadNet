# Signaling (STUN Server)

UDP-based NAT-discovery service. Runs alongside the main TCP server on a separate UDP port. Its sole job is to record each client's external UDP endpoint so peers can be located for P2P.

---

## Overview

shadnet's signaling responsibility is narrow: **endpoint discovery only**. A client pings the STUN server over UDP; the server records the client's external IP:port (as seen from the server) into `udpExt`. That endpoint is later handed out on demand via the TCP `RequestSignalingInfos` command (see matching.md).

The server does **not** drive any connection-state machine. There is no symmetric handshake, no activation-intent registration, and no established/dead events on the shadnet side. All NpMatching2 / NpSignaling connection-state events (ESTABLISHED, DEAD, activation) are produced by the emulator's matching2-signaling layer, not by shadnet.

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
│                        ShadNetServer                         │
│                                                              │
│  ┌──────────────────┐         ┌──────────────────────────┐  │
│  │   TCP Listener   │         │      StunServer (UDP)    │  │
│  │   port 31313     │         │      port 31314          │  │
│  │                  │         │                          │  │
│  │  ClientSession   │ ◄──────►│  HandleStunPing (0x01)   │  │
│  │  └─ RequestSig.  │ shared  │                          │  │
│  └──────────────────┘  state  └──────────────────────────┘  │
│                                                              │
│  ┌────────────────────────────────────────────────────────┐ │
│  │              MatchingSharedState                        │ │
│  │  udpExt:  npid → (external_ip, external_port)          │ │
│  └────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

---

## STUN Server Protocol

The STUN server communicates over raw UDP datagrams. A signaling vport header is stripped first; the remaining payload starts with a 1-byte command identifier. IP addresses are 4 bytes in **network byte order**; ports are 2 bytes in network byte order. NP IDs are 16 bytes, null-padded.

Only one command is handled (`0x01`). Unknown commands are ignored.

---

### HandleStunPing (Command 0x01)

NAT-traversal discovery. The client sends its NP ID and local IP; the server replies with the client's external IP and port as seen from the server.

**Request datagram (21 bytes):**
```
Offset  Size  Field      Description
──────  ────  ─────────  ────────────────────────────────────────
0       1     cmd        0x01
1       16    npid       NP ID, null-padded to 16 bytes
17      4     localIp    Client's local IP (network order)
```

**Reply datagram (6 bytes):**
```
Offset  Size  Field         Description
──────  ────  ────────────  ────────────────────────────────────────
0       4     externalIp    Client's external IP as seen by server (network order)
4       2     externalPort  Client's external port as seen by server (network order)
```

**Side effects:**
- Stores `npid → (externalIp, externalPort)` in `MatchingSharedState.udpExt`.

**Purpose:** Lets the client learn its public-facing UDP endpoint, and lets the server resolve peer endpoints for `RequestSignalingInfos`.

---

## Peer endpoint lookup

Endpoint discovery completes on the TCP side via `RequestSignalingInfos` (matching command 17, documented in matching.md):

1. Client pings the STUN server (UDP 0x01) → its `udpExt` entry is recorded.
2. Client calls `RequestSignalingInfos { target_npid }` (TCP).
3. Server replies with the target's IP/port from `udpExt` (falling back to a room member's stored addr/port).

From there, P2P hole-punching and connection-state tracking happen on the client/emulator side.

---

## Shared State

### udpExt

```
Key:   npid (QString)
Value: (externalIp, externalPort) — QPair<QString, u16>
Lock:  udpLock (QReadWriteLock)
```

Written by `HandleStunPing` (UDP). Read by `RequestSignalingInfos` (TCP) to resolve peer endpoints. An entry is implicitly stale on disconnect and overwritten on the next ping.
