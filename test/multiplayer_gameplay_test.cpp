#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <spawn.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <asio/ip/tcp.hpp>
#include <asio_handle_exception.hpp>
#include <gtest/gtest.h>

#include "diablo.h"
#include "engine/assets.hpp"
#include "helpers/tcp_test_proxy.hpp"
#include "monsters/party_balance.hpp"
#include "quests.h"

extern char **environ;

namespace devilution {
namespace {
struct SessionCase {
	unsigned players;
	bool classic;
	bool delayed;
	uint32_t seed;
	bool privateGame = false;
};

class MultiplayerGameplayTest : public ::testing::Test {
protected:
	std::filesystem::path directory;
	std::array<pid_t, 8> children { };
	unsigned stage = 0;
	SessionCase session { 8, false, false, 19001 };
	bool matrix = false;
	bool retain = false;
	std::array<bool, 8> expectedExit { };
	std::array<std::unique_ptr<TcpTestProxy>, 8> proxies;
	std::array<std::string, 8> results;

	static void SetUpTestSuite()
	{
		LoadCoreArchives();
		LoadGameArchives();
	}
	void SetUp() override
	{
		if (!HaveMainData() || gbIsSpawn)
			GTEST_SKIP() << "Diablo MPQ assets are required for eight-engine gameplay tests";
		const char *evidence = std::getenv("DEVILUTIONX_GAMEPLAY_EVIDENCE_DIR");
		retain = evidence != nullptr && *evidence != '\0';
		const auto root = retain ? std::filesystem::path(evidence) : std::filesystem::temp_directory_path();
		std::filesystem::create_directories(root);
		directory = root / ("devilutionx-gameplay-" + std::to_string(getpid()) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		ASSERT_TRUE(std::filesystem::create_directory(directory));
		std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
		std::ofstream metadata(directory / "scenario.txt");
		metadata << ::testing::UnitTest::GetInstance()->current_test_info()->name() << "\nplayers=" << session.players
		         << " classic=" << session.classic << " delayed=" << session.delayed << " seed=" << session.seed << " private=" << session.privateGame << '\n';
		asio::io_context context;
		asio::ip::tcp::acceptor portReservation(context, { asio::ip::address_v4::loopback(), 0 });
		const std::string port = std::to_string(portReservation.local_endpoint().port());
		portReservation.close();
		for (unsigned id = 0; id < session.players; ++id) {
			const std::string role = std::to_string(id);
			if (session.delayed && id != 0)
				proxies[id] = std::make_unique<TcpTestProxy>(static_cast<uint16_t>(std::stoul(port)), session.seed + id);
			const std::string endpoint = proxies[id] ? std::to_string(proxies[id]->port()) : port;
			const std::string count = std::to_string(session.players);
			const std::string mode = session.classic ? "classic" : "extended";
			const std::string seed = std::to_string(session.seed);
			const std::string privacy = session.privateGame ? "private" : "public";
			const std::string path = directory.string();
			const std::string log = (directory / ("peer-" + role + ".log")).string();
			posix_spawn_file_actions_t actions;
			ASSERT_EQ(posix_spawn_file_actions_init(&actions), 0);
			ASSERT_EQ(posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600), 0);
			ASSERT_EQ(posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO), 0);
			std::vector<char *> arguments { const_cast<char *>(GAMEPLAY_WORKER_PATH), const_cast<char *>(path.c_str()), const_cast<char *>(role.c_str()), const_cast<char *>(endpoint.c_str()) };
			if (matrix) {
				arguments.push_back(const_cast<char *>(count.c_str()));
				arguments.push_back(const_cast<char *>(mode.c_str()));
				arguments.push_back(const_cast<char *>(seed.c_str()));
				arguments.push_back(const_cast<char *>(privacy.c_str()));
			}
			arguments.push_back(nullptr);
			const int status = posix_spawn(&children[id], GAMEPLAY_WORKER_PATH, &actions, nullptr, arguments.data(), environ);
			posix_spawn_file_actions_destroy(&actions);
			ASSERT_EQ(status, 0);
			ASSERT_TRUE(Wait("ready." + role)) << Logs();
		}
	}
	void TearDown() override
	{
		if (directory.empty())
			return;
		if (HasFailure())
			std::cerr << "Eight-engine evidence: " << directory << "\n"
			          << Logs();
		if (!HasFailure()) {
			const auto temporary = directory / "next-command";
			{
				std::ofstream file(temporary);
				file << "quit 0 0";
			}
			std::filesystem::rename(temporary, directory / ("command-" + std::to_string(stage)));
		}
		for (auto &child : children) {
			if (child <= 0)
				continue;
			if (HasFailure())
				kill(child, SIGTERM);
			int status;
			const auto reaped = waitpid(child, &status, 0);
			EXPECT_EQ(reaped, child);
			if (reaped == child && !HasFailure()) {
				EXPECT_EQ(status, 0) << Logs();
			}
		}
		if (retain)
			std::cout << "Session evidence: " << directory << '\n';
		if (!HasFailure() && !retain) {
			std::error_code error;
			std::filesystem::remove_all(directory, error);
		}
	}
	std::string Logs() const
	{
		std::string result;
		for (unsigned id = 0; id < session.players; ++id) {
			std::ifstream log(directory / ("peer-" + std::to_string(id) + ".log"));
			result += "peer " + std::to_string(id) + ":\n" + std::string(std::istreambuf_iterator<char>(log), { });
		}
		return result;
	}
	bool Wait(const std::string &name)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
		while (!std::filesystem::exists(directory / name)) {
			for (unsigned peer = 0; peer < children.size(); ++peer) {
				auto &child = children[peer];
				if (child <= 0)
					continue;
				int status;
				if (waitpid(child, &status, WNOHANG) == child) {
					child = 0;
					if (!expectedExit[peer])
						return false;
					EXPECT_EQ(status, 0) << "departing peer " << peer;
				}
			}
			if (std::chrono::steady_clock::now() >= deadline)
				return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		return true;
	}
	void Command(const std::string &action, unsigned actor = 0, unsigned value = 0)
	{
		const std::string sequence = std::to_string(stage);
		const auto temporary = directory / "next-command";
		{
			std::ofstream file(temporary);
			file << action << ' ' << actor << ' ' << value;
		}
		std::filesystem::rename(temporary, directory / ("command-" + sequence));
		for (unsigned id = 0; id < session.players; ++id)
			ASSERT_TRUE(Wait("sent-" + sequence + "." + std::to_string(id))) << action << '\n'
			                                                                 << Logs();
		{
			std::ofstream file(directory / ("settle-" + sequence));
		}
		for (unsigned id = 0; id < session.players; ++id) {
			const auto result = "result-" + sequence + "." + std::to_string(id);
			ASSERT_TRUE(Wait(result)) << action << '\n'
			                          << Logs();
			std::ifstream file(directory / result);
			results[id] = std::string(std::istreambuf_iterator<char>(file), { });
		}
		++stage;
	}
	void ExpectWorldCount(unsigned expected)
	{
		const auto world = results[0].substr(results[0].find('\n'));
		for (unsigned id = 0; id < session.players; ++id) {
			std::istringstream result(results[id]);
			unsigned count;
			result >> count;
			EXPECT_EQ(count, expected) << "peer " << id << '\n'
			                           << results[id];
			EXPECT_EQ(results[id].substr(results[id].find('\n')), world) << "peer " << id;
		}
	}
	unsigned Held(unsigned role) const
	{
		unsigned floor, empty;
		std::istringstream(results[role]) >> floor >> empty;
		return 1 - empty;
	}
	uint32_t Value(unsigned role, unsigned column) const
	{
		std::istringstream header(results[role]);
		uint32_t value = 0;
		for (unsigned index = 0; index <= column; ++index)
			header >> value;
		return value;
	}
	unsigned Gold(unsigned role) const
	{
		unsigned floor, empty, gold;
		std::istringstream(results[role]) >> floor >> empty >> gold;
		return gold;
	}
};

class MultiplayerSessionTest : public MultiplayerGameplayTest, public ::testing::WithParamInterface<SessionCase> {
protected:
	void SetUp() override
	{
		session = GetParam();
		matrix = true;
		MultiplayerGameplayTest::SetUp();
	}
	void Agree()
	{
		const auto checkpoint = results[0].find("\ncheckpoint ");
		ASSERT_NE(checkpoint, std::string::npos);
		for (unsigned peer = 1; peer < session.players; ++peer) {
			const auto remote = results[peer].find("\ncheckpoint ");
			ASSERT_NE(remote, std::string::npos);
			EXPECT_EQ(results[peer].substr(remote), results[0].substr(checkpoint)) << "peer " << peer << ", stage " << stage << '\n'
			                                                                       << Logs();
		}
	}
	void Step(const std::string &action, unsigned actor = 0, unsigned value = 0)
	{
		ASSERT_NO_FATAL_FAILURE(Command(action, actor, value));
		ASSERT_NO_FATAL_FAILURE(Agree());
	}
};

TEST_P(MultiplayerSessionTest, ConfirmedTurnsKeepWorldStateAcrossSessionSizesAndDelayedStreams)
{
	ASSERT_NO_FATAL_FAILURE(Step("session-init"));
	ASSERT_NO_FATAL_FAILURE(Step("session-entry"));
	ASSERT_NO_FATAL_FAILURE(Step("session-portal"));
	for (unsigned actor = 0; actor < session.players; ++actor)
		ASSERT_NO_FATAL_FAILURE(Step("session-golem", actor));
	if (session.delayed) {
		for (auto &proxy : proxies) {
			if (proxy)
				proxy->Pause(std::chrono::milliseconds(450));
		}
	}
	ASSERT_NO_FATAL_FAILURE(Step("pickup", session.players - 1));
	ExpectWorldCount(1);
	EXPECT_EQ(Gold(session.players - 1), 75U);
	EXPECT_EQ(Held(session.players - 1), 0U);
	ASSERT_NO_FATAL_FAILURE(Step("boss-spawn", 0, 1));
	ASSERT_NO_FATAL_FAILURE(Step("boss-health"));
	ASSERT_NO_FATAL_FAILURE(Step("boss-hit"));
	ASSERT_NO_FATAL_FAILURE(Step("boss-kill", session.players - 1));
	for (unsigned id = 0; id < session.players; ++id)
		EXPECT_GT(Value(id, 4), 0U);
	for (const auto &proxy : proxies) {
		if (proxy) {
			EXPECT_GT(proxy->forwardedBytes(), 0U);
		}
	}
}

class MultiplayerFailureTest : public MultiplayerGameplayTest {
protected:
	void SetUp() override
	{
		matrix = true;
		session = { 8, false, true, 19501, true };
		MultiplayerGameplayTest::SetUp();
	}
	void Interrupt(unsigned peer)
	{
		const std::string sequence = std::to_string(stage);
		{
			std::ofstream file(directory / "next-command");
			file << "session-fault " << peer << " 0";
		}
		std::filesystem::rename(directory / "next-command", directory / ("command-" + sequence));
		for (unsigned id = 0; id < session.players; ++id)
			ASSERT_TRUE(Wait("sent-" + sequence + "." + std::to_string(id))) << Logs();
		if (peer == 0) {
			for (unsigned id = 1; id < session.players; ++id)
				expectedExit[id] = true;
			ASSERT_EQ(kill(children[0], SIGKILL), 0);
			int status;
			ASSERT_EQ(waitpid(children[0], &status, 0), children[0]);
			ASSERT_TRUE(WIFSIGNALED(status));
			EXPECT_EQ(WTERMSIG(status), SIGKILL);
			children[0] = 0;
		} else {
			expectedExit[peer] = true;
			proxies[peer]->Disconnect();
		}
		{
			std::ofstream file(directory / ("settle-" + sequence));
		}
		for (unsigned id = peer == 0 ? 1 : 0; id < session.players; ++id) {
			ASSERT_TRUE(Wait("result-" + sequence + "." + std::to_string(id))) << Logs();
			std::ifstream file(directory / ("result-" + sequence + "." + std::to_string(id)));
			std::string status;
			file >> status;
			EXPECT_EQ(status, peer == 0 || id == peer ? "closed" : "departed");
		}
		++stage;
	}
};

TEST_F(MultiplayerFailureTest, CuttingTheLastPeerCleansItsPortalAndSummonAndTheSurvivorsContinue)
{
	ASSERT_NO_FATAL_FAILURE(Command("session-init"));
	ASSERT_NO_FATAL_FAILURE(Command("session-entry"));
	ASSERT_NO_FATAL_FAILURE(Command("session-portal"));
	ASSERT_NO_FATAL_FAILURE(Command("session-golem", 7));
	ASSERT_NO_FATAL_FAILURE(Interrupt(7));
	--session.players;
	ASSERT_NO_FATAL_FAILURE(Command("measure"));
	ExpectWorldCount(2);
	for (unsigned id = 0; id < session.players; ++id)
		EXPECT_EQ(Value(id, 4), 0U); // Removing a summon must not award experience.
	const auto expected = results[0].substr(results[0].find("\ncheckpoint "));
	for (unsigned id = 1; id < session.players; ++id)
		EXPECT_EQ(results[id].substr(results[id].find("\ncheckpoint ")), expected);
}

TEST_F(MultiplayerFailureTest, CanonicalCheckpointsDetectQuestRngAndSlotGenerationDivergence)
{
	ASSERT_NO_FATAL_FAILURE(Command("session-init"));
	for (unsigned value : { 1U, 2U, 3U }) {
		ASSERT_NO_FATAL_FAILURE(Command("session-diverge", 7, value));
		const auto canonical = [](const std::string &result) { return result.substr(result.find("\ncheckpoint ")); };
		for (unsigned peer = 1; peer < 7; ++peer)
			EXPECT_EQ(canonical(results[peer]), canonical(results[0]));
		EXPECT_NE(canonical(results[7]), canonical(results[0]));
	}
}

TEST_F(MultiplayerFailureTest, KillingTheTcpHostClosesEveryClientsTurnsWithoutInventingMigration)
{
	ASSERT_NO_FATAL_FAILURE(Command("session-init"));
	ASSERT_NO_FATAL_FAILURE(Interrupt(0));
}

class MultiplayerSplitLevelTest : public MultiplayerGameplayTest {
protected:
	void SetUp() override
	{
		matrix = true;
		session = { 8, false, true, 19601, true };
		MultiplayerGameplayTest::SetUp();
	}
	void Step(const std::string &action, unsigned actor = 0, unsigned value = 0)
	{
		ASSERT_NO_FATAL_FAILURE(Command(action, actor, value));
		std::map<std::string, std::string> worlds;
		std::string shared;
		for (unsigned peer = 0; peer < session.players; ++peer) {
			const auto start = results[peer].find("\nturn ");
			const auto end = results[peer].find("end-shared\n", start);
			ASSERT_NE(start, std::string::npos);
			ASSERT_NE(end, std::string::npos);
			const auto global = results[peer].substr(start, end - start);
			if (peer == 0)
				shared = global;
			EXPECT_EQ(global, shared) << "shared state at peer " << peer;
			const auto level = end + std::string("end-shared\n").size();
			const auto key = results[peer].substr(level, results[peer].find('\n', level) - level);
			const auto world = results[peer].substr(level);
			const auto [existing, inserted] = worlds.emplace(key, world);
			if (!inserted) {
				EXPECT_EQ(world, existing->second) << "world state at peer " << peer << ", " << key;
			}
		}
	}
};

TEST_F(MultiplayerSplitLevelTest, SeparateLevelsAndRegroupingKeepConfirmedWorldsAndSnapshotsConsistent)
{
	ASSERT_NO_FATAL_FAILURE(Step("party-init"));
	ASSERT_NO_FATAL_FAILURE(Step("party-spawn"));
	for (unsigned peer = 4; peer < 8; ++peer)
		ASSERT_NO_FATAL_FAILURE(Step("party-move", peer, 1));
	ASSERT_NO_FATAL_FAILURE(Step("party-buffer", 7));
	ASSERT_NO_FATAL_FAILURE(Step("party-export", 7));
}

std::vector<SessionCase> SessionCases()
{
	std::vector<SessionCase> cases;
	for (unsigned players = 1; players <= 8; ++players)
		cases.push_back({ players, false, players == 8, 19000 + players, players == 8 });
	for (unsigned players = 1; players <= 4; ++players)
		cases.push_back({ players, true, players == 4, 19400 + players, players == 4 });
	cases.push_back({ 8, false, true, 19081, true });
	cases.push_back({ 8, false, true, 19082 });
	return cases;
}
INSTANTIATE_TEST_SUITE_P(SessionMatrix, MultiplayerSessionTest, ::testing::ValuesIn(SessionCases()), [](const ::testing::TestParamInfo<SessionCase> &info) {
	return std::string(info.param.classic ? "Classic" : "Extended") + std::to_string(info.param.players) + "Seed" + std::to_string(info.param.seed);
});

TEST_F(MultiplayerGameplayTest, EightEnginesConserveItemsAcrossRemoteRejectionAndLastSlotContention)
{
	ASSERT_NO_FATAL_FAILURE(Command("fill", 7));
	ASSERT_NO_FATAL_FAILURE(Command("pickup", 7));
	ExpectWorldCount(2);
	EXPECT_EQ(Held(7), 1U);
	EXPECT_EQ(Value(7, 7), 8000U);
	ASSERT_NO_FATAL_FAILURE(Command("release", 7));
	ASSERT_NO_FATAL_FAILURE(Command("pickup-race", 7));
	ExpectWorldCount(2);
	EXPECT_EQ(Held(7), 1U);
	EXPECT_EQ(Value(7, 7), 8000U);
	ASSERT_NO_FATAL_FAILURE(Command("release", 7));
	ASSERT_NO_FATAL_FAILURE(Command("pickup", 7));
	ExpectWorldCount(1);
	EXPECT_EQ(Held(7), 1U);
	ASSERT_NO_FATAL_FAILURE(Command("reset", 0, 0));
	for (unsigned role = 0; role < 8; ++role) {
		ASSERT_NO_FATAL_FAILURE(Command("drop", role));
		ExpectWorldCount(role + 1);
		EXPECT_EQ(Held(role), 0U);
	}
	ASSERT_NO_FATAL_FAILURE(Command("reset", 0, 126));
	ASSERT_NO_FATAL_FAILURE(Command("contend"));
	ExpectWorldCount(127);
	unsigned retained = 0;
	for (unsigned role = 0; role < 8; ++role)
		retained += Held(role);
	EXPECT_EQ(retained, 7U);
	unsigned total = Value(0, 5);
	for (unsigned role = 0; role < 8; ++role)
		total += Value(role, 6);
	EXPECT_EQ(total, 126U * 75 + 8U * 123);
}

TEST_F(MultiplayerGameplayTest, EverySlotCanAutopickAndTradeTheSameItemThroughTheFloor)
{
	for (unsigned role = 0; role < 8; ++role) {
		SCOPED_TRACE(role);
		ASSERT_NO_FATAL_FAILURE(Command("reset", 0, 2));
		ASSERT_NO_FATAL_FAILURE(Command("pickup-auto", role));
		ExpectWorldCount(1);
		EXPECT_EQ(Gold(role), 75U);
		EXPECT_EQ(Held(role), 0U);
		ASSERT_NO_FATAL_FAILURE(Command("reset", 0, 2));
		ASSERT_NO_FATAL_FAILURE(Command("fill-inventory", role));
		ASSERT_NO_FATAL_FAILURE(Command("pickup", role));
		ExpectWorldCount(1);
		EXPECT_EQ(Held(role), 1U);
		EXPECT_EQ(Value(role, 7), 1000U);
		ASSERT_NO_FATAL_FAILURE(Command("redrop", role));
		ExpectWorldCount(2);
		EXPECT_EQ(Held(role), 0U);
		EXPECT_EQ(Value(0, 5), 150U);
		const unsigned recipient = (role + 1) % 8;
		ASSERT_NO_FATAL_FAILURE(Command("fill-inventory", recipient));
		ASSERT_NO_FATAL_FAILURE(Command("pickup-dropped", recipient));
		ExpectWorldCount(1);
		EXPECT_EQ(Held(recipient), 1U);
		EXPECT_EQ(Value(recipient, 6), 75U);
		EXPECT_EQ(Value(recipient, 7), 1000U);
		EXPECT_EQ(Value(0, 5), 75U);
	}
}

TEST_F(MultiplayerGameplayTest, EightEnginesKeepDeathOwnershipAndRecoverDeferredQuestRewards)
{
	ASSERT_NO_FATAL_FAILURE(Command("reset", 0, 0));
	for (unsigned role = 0; role < 8; ++role) {
		ASSERT_NO_FATAL_FAILURE(Command("death", role));
		ExpectWorldCount((role + 1) * 2);
		EXPECT_EQ(Gold(role), 2000U);
		ASSERT_NO_FATAL_FAILURE(Command("resurrect", role));
		unsigned floor, empty, gold, hp;
		std::istringstream(results[role]) >> floor >> empty >> gold >> hp;
		EXPECT_GT(hp, 0U);
	}
	ASSERT_NO_FATAL_FAILURE(Command("reset", 0, 127));
	ASSERT_NO_FATAL_FAILURE(Command("death", 6));
	ExpectWorldCount(127);
	EXPECT_EQ(Gold(6), 4000U);
	ASSERT_NO_FATAL_FAILURE(Command("quest", 6));
	ExpectWorldCount(127);
	ASSERT_NO_FATAL_FAILURE(Command("pickup", 7));
	ExpectWorldCount(127);
	ASSERT_NO_FATAL_FAILURE(Command("quest-unique", 6));
	ExpectWorldCount(127);
	ASSERT_NO_FATAL_FAILURE(Command("pickup", 7, 1));
	ExpectWorldCount(127);
	ASSERT_NO_FATAL_FAILURE(Command("quest-book", 6));
	ExpectWorldCount(127);
	ASSERT_NO_FATAL_FAILURE(Command("pickup", 7, 2));
	ExpectWorldCount(127);
	ASSERT_NO_FATAL_FAILURE(Command("reset", 0, 0));
	ASSERT_NO_FATAL_FAILURE(Command("damage"));
	for (unsigned role = 0; role < 8; ++role) {
		unsigned floor, empty, gold, hp;
		std::istringstream(results[role]) >> floor >> empty >> gold >> hp;
		EXPECT_EQ(hp, 99U) << "peer " << role;
	}
}
TEST_F(MultiplayerGameplayTest, ExitingDrainsAcceptedAndRejectedDropsBeforeSavingTheCursor)
{
	ASSERT_NO_FATAL_FAILURE(Command("reset", 0, 0));
	ASSERT_NO_FATAL_FAILURE(Command("drop-finish", 7));
	ExpectWorldCount(1);
	EXPECT_EQ(Held(7), 0U);
	ASSERT_NO_FATAL_FAILURE(Command("reset", 0, 127));
	ASSERT_NO_FATAL_FAILURE(Command("drop-finish", 7));
	ExpectWorldCount(127);
	EXPECT_EQ(Held(7), 1U);
	EXPECT_EQ(Value(7, 6), 123U);
	EXPECT_EQ(Value(7, 7), 9007U);
}

TEST_F(MultiplayerGameplayTest, DeferredRewardsRespectFloorCapacityAfterThePartyChangesLevels)
{
	ASSERT_NO_FATAL_FAILURE(Command("reset", 0, 127));
	ASSERT_NO_FATAL_FAILURE(Command("quest", 6));
	ExpectWorldCount(127);
	ASSERT_NO_FATAL_FAILURE(Command("level", 2, 1));
	ExpectWorldCount(1);
	ASSERT_NO_FATAL_FAILURE(Command("pickup", 7));
	ExpectWorldCount(0);
	ASSERT_NO_FATAL_FAILURE(Command("level", 1, 127));
	ExpectWorldCount(127);
	ASSERT_NO_FATAL_FAILURE(Command("pickup", 7));
	ExpectWorldCount(127);
}

TEST_F(MultiplayerGameplayTest, ADeferredRewardCanMoveWhenItsOriginalTileIsOccupied)
{
	ASSERT_NO_FATAL_FAILURE(Command("reset", 0, 126));
	ASSERT_NO_FATAL_FAILURE(Command("block-reward-position"));
	ASSERT_NO_FATAL_FAILURE(Command("quest", 6));
	ExpectWorldCount(127);
	ASSERT_NO_FATAL_FAILURE(Command("pickup", 7));
	ExpectWorldCount(127);
}

TEST_F(MultiplayerGameplayTest, GeneratedCandidatesReserveCapacityBeforeTheirOffersArrive)
{
	ASSERT_NO_FATAL_FAILURE(Command("reset", 0, 0));
	ASSERT_NO_FATAL_FAILURE(Command("burst", 0));
	ExpectWorldCount(127);
}

TEST_F(MultiplayerGameplayTest, AnEngineRestoresAWorldSnapshotWhileAnItemOfferIsPending)
{
	ASSERT_NO_FATAL_FAILURE(Command("buffer", 7));
	ASSERT_NO_FATAL_FAILURE(Command("drop-sync", 6));
	ASSERT_TRUE(std::filesystem::exists(directory / "captured.0"));
	ASSERT_TRUE(std::filesystem::exists(directory / "restored.7"));
	ExpectWorldCount(3);
	EXPECT_EQ(Held(6), 0U);
}

TEST_F(MultiplayerGameplayTest, EveryPlayerCanHitEveryOtherPlayerAndSharedPoolsRemainIntact)
{
	ASSERT_NO_FATAL_FAILURE(Command("reset", 0, 0));
	ASSERT_NO_FATAL_FAILURE(Command("friendly-off", 0, 1));
	for (unsigned offset = 1; offset < 8; ++offset) {
		ASSERT_NO_FATAL_FAILURE(Command("friendly-on", 0, offset));
		for (unsigned role = 0; role < 8; ++role) {
			unsigned floor, empty, gold, hp;
			std::istringstream(results[role]) >> floor >> empty >> gold >> hp;
			EXPECT_EQ(hp, 100U - offset) << "peer " << role;
		}
	}
	ASSERT_NO_FATAL_FAILURE(Command("resources"));
	ExpectWorldCount(0);
	for (const auto &result : results) {
		std::istringstream common(result.substr(result.find('\n', result.find('\n') + 1) + 1));
		unsigned king, diablo, missiles, objects;
		common >> king >> diablo >> missiles >> objects;
		EXPECT_EQ(missiles, 256U);
		EXPECT_EQ(objects, 127U);
	}
}

TEST_F(MultiplayerGameplayTest, PartyScaleSurvivesLateDamageDeathResurrectionAndSplitGroups)
{
	const auto value = [&](unsigned peer, unsigned column) {
		std::istringstream row(results[peer].substr(results[peer].rfind('\n') + 1));
		uint64_t result = 0;
		for (unsigned i = 0; i <= column; ++i)
			row >> result;
		return result;
	};
	const auto agree = [&]() {
		for (unsigned peer = 1; peer < 8; ++peer)
			EXPECT_EQ(results[peer].substr(results[peer].rfind('\n')), results[0].substr(results[0].rfind('\n'))) << peer;
	};
	ASSERT_NO_FATAL_FAILURE(Command("party-init"));
	ASSERT_NO_FATAL_FAILURE(Command("party-spawn"));
	ASSERT_NO_FATAL_FAILURE(Command("measure"));
	agree();
	EXPECT_EQ(value(0, 0), 4U);
	const uint64_t native = value(0, 2);
	ASSERT_GT(native, 10U * 64);
	ASSERT_NO_FATAL_FAILURE(Command("party-stage-hit", 1));
	ASSERT_NO_FATAL_FAILURE(Command("party-move", 4, 1));
	ASSERT_NO_FATAL_FAILURE(Command("party-release-hit", 1));
	agree();
	EXPECT_EQ(value(0, 0), 5U);
	EXPECT_EQ(value(0, 4), (native - 10 * 64) * 5 / 4);
	const auto after = value(0, 4);
	ASSERT_NO_FATAL_FAILURE(Command("party-buffer", 7));
	ASSERT_NO_FATAL_FAILURE(Command("party-export", 7));
	agree();
	EXPECT_EQ(value(7, 4), after);
	ASSERT_NO_FATAL_FAILURE(Command("party-replay-hit", 1));
	agree();
	EXPECT_EQ(value(0, 4), after);
	ASSERT_NO_FATAL_FAILURE(Command("party-move", 4, 0));
	agree();
	EXPECT_EQ(value(0, 0), 5U);
	EXPECT_EQ(value(0, 4), after);
	ASSERT_NO_FATAL_FAILURE(Command("party-move", 4, 1));
	ASSERT_NO_FATAL_FAILURE(Command("party-dead", 4));
	ASSERT_NO_FATAL_FAILURE(Command("party-move", 5, 1));
	agree();
	EXPECT_EQ(value(0, 0), 6U);
	ASSERT_NO_FATAL_FAILURE(Command("resurrect", 4));
	ASSERT_NO_FATAL_FAILURE(Command("party-move", 6, 1));
	agree();
	EXPECT_EQ(value(0, 0), 7U);
	ASSERT_NO_FATAL_FAILURE(Command("party-move", 7, 1));
	agree();
	EXPECT_EQ(value(0, 0), 8U);
	EXPECT_EQ(value(0, 4), (native - 10 * 64) * 2);
	for (unsigned peer = 1; peer < 8; ++peer)
		ASSERT_NO_FATAL_FAILURE(Command("party-move", peer, 0));
	agree();
	EXPECT_EQ(value(0, 0), 8U);
	EXPECT_EQ(value(0, 4), (native - 10 * 64) * 2);
	ASSERT_NO_FATAL_FAILURE(Command("party-kill", 0));
	agree();
	EXPECT_EQ(value(0, 4), 0U);
	EXPECT_NE(value(0, 6) & PartyMonsterRewarded, 0U);
	const auto experience = Value(0, 4);
	EXPECT_GT(experience, 0U);
	ASSERT_NO_FATAL_FAILURE(Command("party-replay-hit", 1));
	EXPECT_EQ(Value(0, 4), experience);
	ASSERT_NO_FATAL_FAILURE(Command("party-move", 4, 1));
	agree();
	EXPECT_EQ(value(0, 4), 0U);
	EXPECT_EQ(Value(0, 4), experience);
	ASSERT_NO_FATAL_FAILURE(Command("party-buffer", 7));
	ASSERT_NO_FATAL_FAILURE(Command("party-export", 7));
	agree();
	ASSERT_NO_FATAL_FAILURE(Command("party-replay-hit", 1));
	EXPECT_EQ(Value(0, 4), experience);
	EXPECT_EQ(value(7, 4), 0U);
}

TEST_F(MultiplayerGameplayTest, EightMeleeAttackersShareBossDeathsLootAndQuestCompletion)
{
	for (unsigned type : { 1U, 2U }) {
		SCOPED_TRACE(type);
		ASSERT_NO_FATAL_FAILURE(Command("reset", 0, 0));
		ASSERT_NO_FATAL_FAILURE(Command("boss-types"));
		ASSERT_NO_FATAL_FAILURE(Command("boss-spawn", 0, type));
		ASSERT_NO_FATAL_FAILURE(Command("boss-health"));
		ASSERT_NO_FATAL_FAILURE(Command("boss-hit"));
		ExpectWorldCount(0);
		for (const auto &result : results) {
			std::istringstream common(result.substr(result.find('\n', result.find('\n') + 1) + 1));
			unsigned king, diablo, missiles, objects, hp, mask;
			common >> king >> diablo >> missiles >> objects >> hp >> mask;
			EXPECT_GT(hp, 0U);
			EXPECT_LT(hp, 100U);
			EXPECT_EQ(mask, 255U);
		}
		ASSERT_NO_FATAL_FAILURE(Command("boss-kill", 7));
		ExpectWorldCount(static_cast<unsigned>(std::stoul(results[0])));
		for (const auto &result : results) {
			std::istringstream common(result.substr(result.find('\n', result.find('\n') + 1) + 1));
			unsigned king, diablo, missiles, objects, hp, mask;
			common >> king >> diablo >> missiles >> objects >> hp >> mask;
			EXPECT_EQ(hp, 0U);
			EXPECT_EQ(mask, 255U);
			EXPECT_EQ(type == 1 ? king : diablo, QUEST_DONE);
			unsigned floor, empty, gold, playerHp, experience;
			std::istringstream(result) >> floor >> empty >> gold >> playerHp >> experience;
			EXPECT_GT(experience, 0U);
		}
	}
}

} // namespace
} // namespace devilution
