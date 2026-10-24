#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
#include "quests.h"

extern char **environ;

namespace devilution {
namespace {
class MultiplayerGameplayTest : public ::testing::Test {
protected:
	std::filesystem::path directory;
	std::array<pid_t, 8> children { };
	unsigned stage = 0;
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
		directory = std::filesystem::temp_directory_path() / ("devilutionx-gameplay-" + std::to_string(getpid()) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		ASSERT_TRUE(std::filesystem::create_directory(directory));
		asio::io_context context;
		asio::ip::tcp::acceptor portReservation(context, { asio::ip::address_v4::loopback(), 0 });
		const std::string port = std::to_string(portReservation.local_endpoint().port());
		portReservation.close();
		for (unsigned id = 0; id < children.size(); ++id) {
			const std::string role = std::to_string(id);
			const std::string path = directory.string();
			const std::string log = (directory / ("peer-" + role + ".log")).string();
			posix_spawn_file_actions_t actions;
			ASSERT_EQ(posix_spawn_file_actions_init(&actions), 0);
			ASSERT_EQ(posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600), 0);
			ASSERT_EQ(posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO), 0);
			std::array<char *, 5> arguments { const_cast<char *>(GAMEPLAY_WORKER_PATH), const_cast<char *>(path.c_str()), const_cast<char *>(role.c_str()), const_cast<char *>(port.c_str()), nullptr };
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
		if (!HasFailure()) {
			std::error_code error;
			std::filesystem::remove_all(directory, error);
		}
	}
	std::string Logs() const
	{
		std::string result;
		for (unsigned id = 0; id < children.size(); ++id) {
			std::ifstream log(directory / ("peer-" + std::to_string(id) + ".log"));
			result += "peer " + std::to_string(id) + ":\n" + std::string(std::istreambuf_iterator<char>(log), { });
		}
		return result;
	}
	bool Wait(const std::string &name)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
		while (!std::filesystem::exists(directory / name)) {
			for (auto &child : children) {
				if (child <= 0)
					continue;
				int status;
				if (waitpid(child, &status, WNOHANG) == child) {
					child = 0;
					return false;
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
		for (unsigned id = 0; id < children.size(); ++id)
			ASSERT_TRUE(Wait("sent-" + sequence + "." + std::to_string(id))) << action << '\n'
			                                                                 << Logs();
		{
			std::ofstream file(directory / ("settle-" + sequence));
		}
		for (unsigned id = 0; id < children.size(); ++id) {
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
		for (unsigned id = 0; id < children.size(); ++id) {
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
