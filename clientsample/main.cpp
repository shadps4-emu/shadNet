#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include "client.h"

// Usage:
//   shadnet-sample <host> <port> register   <npid> <password> <onlineName> <email>
//   shadnet-sample <host> <port> login      <npid> <password> [token]
//   shadnet-sample <host> <port> friend-add    <npid> <password> <friend_npid>
//   shadnet-sample <host> <port> friend-remove <npid> <password> <friend_npid>
//   shadnet-sample <host> <port> block-add     <npid> <password> <target_npid>
//   shadnet-sample <host> <port> block-remove  <npid> <password> <target_npid>
//
// Friend/block commands authenticate first, then issue the friend command,
// and keep polling so any incoming notifications are also printed.

static void printUsage(const char* prog)
{
	printf("Usage:\n"
		"  %s <host> <port> register     <npid> <password> <onlineName> <email>\n"
		"  %s <host> <port> login        <npid> <password> [token]\n"
		"  %s <host> <port> friend-add   <npid> <password> <friend_npid>\n"
		"  %s <host> <port> friend-remove <npid> <password> <friend_npid>\n"
		"  %s <host> <port> block-add    <npid> <password> <target_npid>\n"
		"  %s <host> <port> block-remove <npid> <password> <target_npid>\n",
		prog, prog, prog, prog, prog, prog);
}

// Poll conn until 'done' is true or timeout (ms) expires.
// Returns true if done was set before timeout.
static bool pollUntil(shadnet::ShadNetClient& client, bool& done, int timeoutMs = 10000)
{
	for (int i = 0; i < timeoutMs / 10 && !done; ++i) {
		client.update();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return done;
}

int main(int argc, char* argv[])
{
	if (argc < 4) { printUsage(argv[0]); return 1; }

	const char* host = argv[1];
	uint16_t    port = static_cast<uint16_t>(atoi(argv[2]));
	const char* command = argv[3];

	shadnet::ShadNetClient client;

	printf("[connect] %s:%u ...\n", host, port);
	if (!client.connect(host, port)) {
		printf("[connect] FAILED: %s\n", client.lastError().c_str());
		return 1;
	}
	printf("[connect] OK (protocol v%u)\n", shadnet::PROTOCOL_VERSION);

	//register

	if (strcmp(command, "register") == 0)
	{
		if (argc < 8) {
			printf("register requires: <npid> <password> <onlineName> <email>\n");
			return 1;
		}
		bool done = false;
		client.onCreateResult = [&done](shadnet::ErrorType) { done = true; };
		client.createAccount(argv[4], argv[5], argv[6], "", argv[7]);
		if (!pollUntil(client, done))
			printf("[timeout] No reply received.\n");
		client.disconnect();
		return done ? 0 : 1;
	}

	//login (standalone — just shows friend lists)
	if (strcmp(command, "login") == 0)
	{
		if (argc < 6) {
			printf("login requires: <npid> <password> [token]\n");
			return 1;
		}
		const char* token = (argc >= 7) ? argv[6] : "";
		bool done = false;
		client.onLoginResult = [&done](const shadnet::LoginResult&) { done = true; };
		client.login(argv[4], argv[5], token);
		if (!pollUntil(client, done))
			printf("[timeout] No reply received.\n");
		client.disconnect();
		return done ? 0 : 1;
	}

	// ── friend-add / friend-remove / block-add / block-remove
	// All four follow the same pattern:
	//   1. Login
	//   2. Wait for login reply
	//   3. If login succeeded, send the friend command
	//   4. Wait for the friend command reply
	//   5. Keep polling a bit longer to catch any incoming notifications

	bool isFriendCmd = (strcmp(command, "friend-add") == 0 ||
		strcmp(command, "friend-remove") == 0 ||
		strcmp(command, "block-add") == 0 ||
		strcmp(command, "block-remove") == 0);

	if (!isFriendCmd) {
		printf("Unknown command: %s\n", command);
		printUsage(argv[0]);
		return 1;
	}

	if (argc < 7) {
		printf("%s requires: <npid> <password> <target_npid>\n", command);
		return 1;
	}

	const char* npid = argv[4];
	const char* password = argv[5];
	const char* targetNpid = argv[6];

	bool loginDone = false;
	bool loginOk = false;
	bool friendDone = false;

	client.onLoginResult = [&](const shadnet::LoginResult& res) {
		loginOk = (res.error == shadnet::ErrorType::NoError);
		loginDone = true;
		};

	client.onFriendResult = [&](const shadnet::FriendResult&) {
		friendDone = true;
		};

	// login
	client.login(npid, password, "");
	if (!pollUntil(client, loginDone)) {
		printf("[timeout] No login reply.\n");
		client.disconnect();
		return 1;
	}
	if (!loginOk) {
		client.disconnect();
		return 1;
	}

	//  send the friend command
	if (strcmp(command, "friend-add") == 0) client.addFriend(targetNpid);
	else if (strcmp(command, "friend-remove") == 0) client.removeFriend(targetNpid);
	else if (strcmp(command, "block-add") == 0) client.addBlock(targetNpid);
	else                                            client.removeBlock(targetNpid);

	if (!pollUntil(client, friendDone)) {
		printf("[timeout] No friend command reply.\n");
		client.disconnect();
		return 1;
	}

	// poll a bit longer so any incoming FriendQuery/FriendNew
	// notifications from the other side are printed before we exit.
	for (int i = 0; i < 50; ++i) {
		client.update();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	client.disconnect();
	return 0;
}
