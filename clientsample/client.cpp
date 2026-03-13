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

	std::vector<uint8_t> out;
	out.resize(HEADER_SIZE);

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
	const std::string& npid,
	const std::string& password,
	const std::string& onlineName,
	const std::string& avatarUrl,
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
			auto readStr = [&]() {
				std::string s;
				while (pos < (int)payload.size() && payload[pos] != 0)
					s += static_cast<char>(payload[pos++]);
				if (pos < (int)payload.size()) ++pos;
				return s;
				};
			res.onlineName = readStr();
			res.avatarUrl = readStr();
			if (pos + 8 <= (int)payload.size()) {
				res.userId = fromLE64(payload.data() + pos);
				pos += 8;
			}
		}
	}

	if (res.error == ErrorType::NoError)
		printf("[login] OK  onlineName=%s  userId=%llu\n",
			res.onlineName.c_str(),
			static_cast<unsigned long long>(res.userId));
	else
		printf("[login] FAILED: %s\n", errorName(res.error));

	if (onLoginResult) onLoginResult(res);
}

void ShadNetClient::handleCreateReply(const std::vector<uint8_t>& payload)
{
	ErrorType err = payload.empty()
		? ErrorType::Malformed
		: static_cast<ErrorType>(payload[0]);

	if (err == ErrorType::NoError)
		printf("[create] Account created successfully.\n");
	else
		printf("[create] FAILED: %s\n", errorName(err));

	if (onCreateResult) onCreateResult(err);
}

void ShadNetClient::handlePacket(const Packet& pkt)
{
	if (pkt.type == PacketType::ServerInfo)
	{
		return;
	}

	if (pkt.type != PacketType::Reply)
		return;

	switch (static_cast<CommandType>(pkt.command))
	{
	case CommandType::Login:
		handleLoginReply(pkt.payload);
		break;
	case CommandType::Create:
		handleCreateReply(pkt.payload);
		break;
	default:
		printf("[recv] Unknown reply command=%u\n", pkt.command);
		break;
	}
}
