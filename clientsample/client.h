#pragma once
#include "connection.h"
#include <string>
#include <vector>
#include <atomic>
#include <functional>

namespace shadnet
{
	// ── Result types ──────────────────────────────────────────────────────────

	struct FriendEntry
	{
		std::string npid;
		bool        online = false;
	};

	struct LoginResult
	{
		ErrorType   error = ErrorType::Malformed;
		std::string onlineName;
		std::string avatarUrl;
		uint64_t    userId = 0;

		// Populated on success.
		std::vector<FriendEntry> friends;
		std::vector<std::string> friendRequestsSent;
		std::vector<std::string> friendRequestsReceived;
		std::vector<std::string> blocked;
	};

	// Generic single-error result used by all friend commands.
	struct FriendResult
	{
		CommandType cmd;
		ErrorType   error = ErrorType::Malformed;
		std::string targetNpid; // the npid we operated on
	};

	// Notification structs pushed by the server unsolicited.
	struct NotifyFriendQuery { std::string fromNpid; };
	struct NotifyFriendNew { std::string npid; bool online; };
	struct NotifyFriendLost { std::string npid; };
	struct NotifyFriendStatus { std::string npid; bool online; uint64_t timestamp; };

	// ── Client ────────────────────────────────────────────────────────────────

	class ShadNetClient
	{
	public:
		bool connect(const char* host, uint16_t port);
		void disconnect();
		void update();

		// ── Account commands ──────────────────────────────────────────────────
		void login(const std::string& npid,
			const std::string& password,
			const std::string& token = {});

		void createAccount(const std::string& npid,
			const std::string& password,
			const std::string& onlineName,
			const std::string& avatarUrl,
			const std::string& email);

		// ── Friend commands (require prior successful login) ───────────────────
		void addFriend(const std::string& targetNpid);
		void removeFriend(const std::string& targetNpid);
		void addBlock(const std::string& targetNpid);
		void removeBlock(const std::string& targetNpid);

		// ── Callbacks ─────────────────────────────────────────────────────────
		std::function<void(const LoginResult&)>  onLoginResult;
		std::function<void(ErrorType)>           onCreateResult;
		std::function<void(const FriendResult&)> onFriendResult;

		std::function<void(const NotifyFriendQuery&)>  onFriendQuery;
		std::function<void(const NotifyFriendNew&)>    onFriendNew;
		std::function<void(const NotifyFriendLost&)>   onFriendLost;
		std::function<void(const NotifyFriendStatus&)> onFriendStatus;

		const std::string& lastError() const { return conn.lastError(); }

	private:
		ShadNetConnection     conn;
		std::atomic<uint64_t> packetCounter{ 1 };

		std::vector<uint8_t> buildPacket(CommandType cmd, uint64_t id,
			const std::vector<uint8_t>& payload);
		void sendFriendCommand(CommandType cmd, const std::string& targetNpid);

		void handlePacket(const Packet& pkt);
		void handleLoginReply(const std::vector<uint8_t>& payload);
		void handleCreateReply(const std::vector<uint8_t>& payload);
		void handleFriendReply(CommandType cmd, const std::vector<uint8_t>& payload);
		void handleNotification(const Packet& pkt);

		// Tracks the in-flight friend command so the reply handler can echo
		// the target npid back through onFriendResult.
		std::string m_pendingFriendNpid;
		CommandType m_pendingFriendCmd = CommandType::AddFriend;
	};
}
