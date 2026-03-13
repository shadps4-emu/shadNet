#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include "client.h"

// Usage:
//   shadnet-sample <host> <port> register <npid> <password> <onlineName> <email>
//   shadnet-sample <host> <port> login    <npid> <password> [token]

int main(int argc, char* argv[])
{
	if (argc < 4) {
		printf("Usage:\n"
			"  %s <host> <port> register <npid> <password> <onlineName> <email>\n"
			"  %s <host> <port> login    <npid> <password> [token]\n",
			argv[0], argv[0]);
		return 1;
	}

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

	bool done = false;

	if (strcmp(command, "register") == 0)
	{
		if (argc < 8) {
			printf("register requires: <npid> <password> <onlineName> <email>\n");
			return 1;
		}
		client.onCreateResult = [&done](shadnet::ErrorType) { done = true; };
		client.createAccount(argv[4], argv[5], argv[6], "", argv[7]);
	}
	else if (strcmp(command, "login") == 0)
	{
		if (argc < 6) {
			printf("login requires: <npid> <password> [token]\n");
			return 1;
		}
		const char* token = (argc >= 7) ? argv[6] : "";
		client.onLoginResult = [&done](const shadnet::LoginResult&) { done = true; };
		client.login(argv[4], argv[5], token);
	}
	else
	{
		printf("Unknown command: %s\n", command);
		return 1;
	}

	// Poll until we get the reply (max 10 seconds)
	for (int i = 0; i < 1000 && !done; ++i) {
		client.update();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	if (!done)
		printf("[timeout] No reply received.\n");

	client.disconnect();
	return done ? 0 : 1;
}
