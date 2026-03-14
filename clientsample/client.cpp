#include "client.h"
#include <cstring>
#include <cstdio>

using namespace shadnet;

bool ShadNetClient::connect(const char* host, uint16_t port)
{
	if (!conn.connect(host, port))
		return false;
	conn.onPacket = [this](const Packet& pkt) { handlePacket(pkt); };
	return true;
}

void ShadNetClient::disconnect() { conn.disconnect(); }
void ShadNetClient::update() { conn.update(); }

std::vector<uint8_t> ShadNetClient::buildPacket(
	CommandType cmd, uint64_t id, const std::vector<uint8_t>& payload)
{
	uint32_t totalSize = static_cast<uint32_t>(HEADER_SIZE + payload.size());
	std::vector<uint8_t> out(HEADER_SIZE);

	out[0] = static_cast<uint8_t>(PacketType::Request);
	uint16_t cmdLE = toLE16(static_cast<uint16_t>(cmd));
	uint32_t sizeLE = toLE32(totalSize);
	uint64_t idLE = toLE64(id);
	memcpy(out.data() + 1, &cmdLE, 2);
	memcpy(out.data() + 3, &sizeLE, 4);
	memcpy(out.data() + 7, &idLE, 8);

	out.insert(out.end(), payload.begin(), payload.end());
	return out;
}

// ── Account commands ──────────────────────────────────────────────────────────

void ShadNetClient::login(const std::string& npid,
	const std::string& password,
	const std::string& token)
{
	std::vector<uint8_t> payload;
	auto addStr = [&](const std::string& s) {
		payload.insert(payload.end(), s.begin(), s.end());
		payload.push_back(0);
		};
	addStr(npid);
	addStr(password);
	addStr(token);
	conn.send(buildPacket(CommandType::Login, packetCounter++, payload));
}

void ShadNetClient::createAccount(
	const std::string& npid, const std::string& password,
	const std::string& onlineName, const std::string& avatarUrl,
	const std::string& email)
{
	std::vector<uint8_t> payload;
	auto addStr = [&](const std::string& s) {
		payload.insert(payload.end(), s.begin(), s.end());
		payload.push_back(0);
		};
	addStr(npid);
	addStr(password);
	addStr(onlineName);
	addStr(avatarUrl);
	addStr(email);
	conn.send(buildPacket(CommandType::Create, packetCounter++, payload));
}

//Friend commands
void ShadNetClient::sendFriendCommand(CommandType cmd, const std::string& targetNpid)
{
	m_pendingFriendCmd = cmd;
	m_pendingFriendNpid = targetNpid;

	std::vector<uint8_t> payload;
	payload.insert(payload.end(), targetNpid.begin(), targetNpid.end());
	payload.push_back(0);
	conn.send(buildPacket(cmd, packetCounter++, payload));
}

void ShadNetClient::addFriend(const std::string& npid) { sendFriendCommand(CommandType::AddFriend, npid); }
void ShadNetClient::removeFriend(const std::string& npid) { sendFriendCommand(CommandType::RemoveFriend, npid); }
void ShadNetClient::addBlock(const std::string& npid) { sendFriendCommand(CommandType::AddBlock, npid); }
void ShadNetClient::removeBlock(const std::string& npid) { sendFriendCommand(CommandType::RemoveBlock, npid); }

// ── Reply handlers ────────────────────────────────────────────────────────────

// Presence blob layout:
//   ComId(12) + title\0 + status\0 + comment\0 + data_len(u32 LE) + data
// We skip it entirely — presence display is not implemented in this client.
static bool skipPresence(const std::vector<uint8_t>& p, int& pos)
{
	if (pos + 12 > (int)p.size()) return false;
	pos += 12; // ComId
	for (int i = 0; i < 3; ++i) { // three null-terminated strings
		while (pos < (int)p.size() && p[pos] != 0) ++pos;
		if (pos >= (int)p.size()) return false;
		++pos;
	}
	if (pos + 4 > (int)p.size()) return false;
	uint32_t dataLen = fromLE32(p.data() + pos);
	pos += 4 + static_cast<int>(dataLen);
	return pos <= (int)p.size();
}

void ShadNetClient::handleLoginReply(const std::vector<uint8_t>& payload)
{
	LoginResult res;
	if (payload.empty()) { res.error = ErrorType::Malformed; }
	else
	{
		res.error = static_cast<ErrorType>(payload[0]);
		if (res.error == ErrorType::NoError)
		{
			int pos = 1;

			auto readStr = [&]() -> std::string {
				std::string s;
				while (pos < (int)payload.size() && payload[pos] != 0)
					s += static_cast<char>(payload[pos++]);
				if (pos < (int)payload.size()) ++pos;
				return s;
				};
			auto readU32 = [&]() -> uint32_t {
				if (pos + 4 > (int)payload.size()) return 0;
				uint32_t v = fromLE32(payload.data() + pos);
				pos += 4; return v;
				};

			res.onlineName = readStr();
			res.avatarUrl = readStr();
			if (pos + 8 <= (int)payload.size()) {
				res.userId = fromLE64(payload.data() + pos);
				pos += 8;
			}

			// Friends: count(u32) then for each: npid\0 + online(u8) + presence
			uint32_t fc = readU32();
			for (uint32_t i = 0; i < fc && pos < (int)payload.size(); ++i) {
				FriendEntry fe;
				fe.npid = readStr();
				fe.online = (pos < (int)payload.size()) && (payload[pos++] != 0);
				skipPresence(payload, pos);
				res.friends.push_back(fe);
			}

			uint32_t sc = readU32();
			for (uint32_t i = 0; i < sc; ++i) res.friendRequestsSent.push_back(readStr());

			uint32_t rc = readU32();
			for (uint32_t i = 0; i < rc; ++i) res.friendRequestsReceived.push_back(readStr());

			uint32_t bc = readU32();
			for (uint32_t i = 0; i < bc; ++i) res.blocked.push_back(readStr());
		}
	}

	if (res.error == ErrorType::NoError) {
		printf("[login] OK  onlineName=%s  userId=%llu\n",
			res.onlineName.c_str(),
			static_cast<unsigned long long>(res.userId));

		printf("[login]   friends(%zu):", res.friends.size());
		for (const auto& f : res.friends)
			printf("  %s(%s)", f.npid.c_str(), f.online ? "online" : "offline");
		printf(res.friends.empty() ? "  (none)\n" : "\n");

		if (!res.friendRequestsSent.empty()) {
			printf("[login]   requests_sent:");
			for (const auto& n : res.friendRequestsSent) printf("  %s", n.c_str());
			printf("\n");
		}
		if (!res.friendRequestsReceived.empty()) {
			printf("[login]   requests_received:");
			for (const auto& n : res.friendRequestsReceived) printf("  %s", n.c_str());
			printf("\n");
		}
		if (!res.blocked.empty()) {
			printf("[login]   blocked:");
			for (const auto& n : res.blocked) printf("  %s", n.c_str());
			printf("\n");
		}
	}
	else {
		printf("[login] FAILED: %s\n", errorName(res.error));
	}

	if (onLoginResult) onLoginResult(res);
}

void ShadNetClient::handleCreateReply(const std::vector<uint8_t>& payload)
{
	ErrorType err = payload.empty() ? ErrorType::Malformed
		: static_cast<ErrorType>(payload[0]);
	if (err == ErrorType::NoError) printf("[create] Account created successfully.\n");
	else                           printf("[create] FAILED: %s\n", errorName(err));
	if (onCreateResult) onCreateResult(err);
}

void ShadNetClient::handleFriendReply(CommandType cmd,
	const std::vector<uint8_t>& payload)
{
	static const char* names[] = {
		"login","terminate","create","delete",
		"send-token","send-reset-token","reset-password","reset-state",
		"add-friend","remove-friend","add-block","remove-block"
	};
	int idx = static_cast<int>(cmd);
	const char* name = (idx >= 0 && idx < 12) ? names[idx] : "friend-cmd";

	FriendResult res;
	res.cmd = cmd;
	res.targetNpid = m_pendingFriendNpid;
	res.error = payload.empty() ? ErrorType::Malformed
		: static_cast<ErrorType>(payload[0]);

	if (res.error == ErrorType::NoError)
		printf("[%s] OK  target=%s\n", name, res.targetNpid.c_str());
	else
		printf("[%s] FAILED: %s  target=%s\n",
			name, errorName(res.error), res.targetNpid.c_str());

	if (onFriendResult) onFriendResult(res);
}

//Notification handler
void ShadNetClient::handleNotification(const Packet& pkt)
{
	const auto& p = pkt.payload;
	int pos = 0;

	auto readStr = [&]() -> std::string {
		std::string s;
		while (pos < (int)p.size() && p[pos] != 0) s += static_cast<char>(p[pos++]);
		if (pos < (int)p.size()) ++pos;
		return s;
		};

	switch (static_cast<NotificationType>(pkt.command))
	{
	case NotificationType::FriendQuery:
	{
		NotifyFriendQuery n;
		n.fromNpid = readStr();
		printf("[notify] FriendQuery from %s\n", n.fromNpid.c_str());
		if (onFriendQuery) onFriendQuery(n);
		break;
	}
	case NotificationType::FriendNew:
	{
		NotifyFriendNew n;
		if (pos < (int)p.size()) n.online = (p[pos++] != 0);
		n.npid = readStr();
		printf("[notify] FriendNew %s (%s)\n", n.npid.c_str(), n.online ? "online" : "offline");
		if (onFriendNew) onFriendNew(n);
		break;
	}
	case NotificationType::FriendLost:
	{
		NotifyFriendLost n;
		n.npid = readStr();
		printf("[notify] FriendLost %s\n", n.npid.c_str());
		if (onFriendLost) onFriendLost(n);
		break;
	}
	case NotificationType::FriendStatus:
	{
		NotifyFriendStatus n;
		if (pos < (int)p.size()) n.online = (p[pos++] != 0);
		if (pos + 8 <= (int)p.size()) { n.timestamp = fromLE64(p.data() + pos); pos += 8; }
		n.npid = readStr();
		printf("[notify] FriendStatus %s is %s\n",
			n.npid.c_str(), n.online ? "online" : "offline");
		if (onFriendStatus) onFriendStatus(n);
		break;
	}
	default:
		printf("[notify] Unknown type=%u\n", pkt.command);
		break;
	}
}

void ShadNetClient::handlePacket(const Packet& pkt)
{
	if (pkt.type == PacketType::Notify) {
		handleNotification(pkt);
		return;
	}
	if (pkt.type == PacketType::ServerInfo) return;
	if (pkt.type != PacketType::Reply)      return;

	switch (static_cast<CommandType>(pkt.command))
	{
	case CommandType::Login:        handleLoginReply(pkt.payload);  break;
	case CommandType::Create:       handleCreateReply(pkt.payload); break;
	case CommandType::AddFriend:
	case CommandType::RemoveFriend:
	case CommandType::AddBlock:
	case CommandType::RemoveBlock:
		handleFriendReply(static_cast<CommandType>(pkt.command), pkt.payload);
		break;
	default:
		printf("[recv] Unhandled reply command=%u\n", pkt.command);
		break;
	}
}
