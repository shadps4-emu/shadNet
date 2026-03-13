#pragma once
#include "connection.h"
#include <string>   
#include <vector>     
#include <atomic>
#include <functional>

namespace shadnet
{
	struct LoginResult
	{
		ErrorType   error = ErrorType::Malformed;
		std::string onlineName;
		std::string avatarUrl;
		uint64_t    userId = 0;
	};

	class ShadNetClient
	{
	public:
		// Connect and complete the ServerInfo handshake.
		bool connect(const char* host, uint16_t port);
		void disconnect();

		// Non-blocking poll — call from your main/update loop.
		void update();

		// Send a Login request.  Result delivered via onLoginResult callback.
		void login(const std::string& npid,
			const std::string& password,
			const std::string& token = {});

		// Send a Create request.  Result delivered via onCreateResult callback.
		void createAccount(const std::string& npid,
			const std::string& password,
			const std::string& onlineName,
			const std::string& avatarUrl,
			const std::string& email);

		// Optional callbacks — set before calling login/createAccount.
		std::function<void(const LoginResult&)>  onLoginResult;
		std::function<void(ErrorType)>           onCreateResult;

		const std::string& lastError() const { return conn.lastError(); }

	private:
		ShadNetConnection     conn;
		std::atomic<uint64_t> packetCounter{ 1 };

		// Assemble a complete request packet (header + payload).
		std::vector<uint8_t> buildPacket(CommandType cmd,
			uint64_t id,
			const std::vector<uint8_t>& payload);

		void handlePacket(const Packet& pkt);
		void handleLoginReply(const std::vector<uint8_t>& payload);
		void handleCreateReply(const std::vector<uint8_t>& payload);
	};
}
