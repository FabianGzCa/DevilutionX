# Extended multiplayer games

DevilutionX can host multiplayer games for up to eight players. These are called
*Extended* games. *Classic* games keep the original four-player limit and stay
compatible with other DevilutionX versions. Classic is the default.

## Creating a game

1. Choose **Multi Player**, then **Client-Server (TCP)** or **ZeroTier**.
2. Choose **Create Game** to set a password, or **Create Public Game** for a game
   without one.
3. In **Select Game Mode**, choose **Classic** (up to four players) or
   **Extended** (up to eight players).
4. Choose the difficulty and the game speed as usual.

Extended games need a network provider; the offline (loopback) mode only hosts
a single player. With ZeroTier, the list of public games shows the mode of each
game and how many slots are taken, for example `Mode: Extended` and
`Players (5/8)`.

## Joining a game

Choose **Join Game** and enter the host's address, or pick a game from the
public list (ZeroTier). You do not have to know the mode: the client asks to join as a
classic game first and, if the host runs an extended game, tries again in that
mode. A full game reports that it is full. A game you cannot join reports why:
a different version, a different edition, or a different mode.

## What every player needs

- The same DevilutionX version (major, minor and patch).
- The same edition: Diablo, Hellfire or the shareware version.
- A build that supports extended games. Older builds cannot join an extended
  game; they see a generic error such as `Unknown packet type 23` or a closed
  connection. Extended builds still join classic games hosted by older builds.

## How the game adapts to the party

In an extended game, monsters adapt to the number of players on their level:
the players there now, including dead ones and those still loading the level.
From the second player on, each player adds half of the native health,
experience and random loot rolls of every monster: one player sees the native
values, two see 1.5 times, four 2.5 times and eight 4.5 times. Diablo instead
grows with every player (eight players face eight times his health), so the
final fight lasts as long as it does alone.

- **Experience:** the larger pool is shared among the players who hit each
  monster, as before. Each player therefore gains experience at about the same
  pace as alone, whether the party has two or eight players.
- **Loot:** each player finds random items and gold at about the same pace as
  alone. Fixed quest rewards are not duplicated.
- **Speed:** a party still clears faster than one player, but not trivially:
  two players need about 75% of the solo time per monster, four about 63% and
  eight about 56%. Monster damage, accuracy and numbers stay native, and in
  automated tests parties of two to eight died less often than one player.
- **Arrivals and departures:** when someone enters or leaves the level,
  returns to town or disconnects, every monster's health is adjusted at once
  and keeps its percentage: a monster at half health stays at half health.

Classic games keep the original rules for one to four players. The exact
rules, and the measurements behind them, are in
[Extended party balance](extended-party-balance.md).

Extended games agree on every monster death between all players. In a classic
game, a hit from another player that would kill a monster only lowers it to one
hit point, until a hit in the attacker's own game kills it. Groups that strike
together therefore finish monsters slightly sooner in extended games, even with
four players.

## Telling players apart

Players five to eight reuse the original four automap and outline colors. To
number every player on the automap, the party portraits and the monster health
bar tags, enable **Settings → Gameplay → Player Identifiers**. It only changes
your own display. See [Optional player identifiers](player-identifiers.md).

## Limitations

- **No host migration.** In a TCP game the host is the server: when the host
  leaves, the game ends for everyone, as in classic games. With ZeroTier, a
  player who cannot reach another player is dropped from the game.
- If a player disconnects in the middle of an item transfer, the game ends
  instead of guessing whether the item moved. See
  [Extended item transfers](extended-item-transfers.md).
- When someone enters a level, the game on that level pauses until every player
  there has agreed on where the newcomer stands. With eight players on one
  computer this took about half a second; slower connections take longer.
- The balance was measured with automated players, not with groups of people.
- Players need builds with the same balance revision. A build with the first
  revision, where monsters only grew with five or more players, cannot join.

## Saves and settings

Hero saves keep their format, and an extended game does not change the classic
game saves. In multiplayer, a hero save may also store the item held on the
cursor in an optional `helditem` entry; older builds ignore it. Place that item
in your inventory before playing the hero with an older build. To back up a
hero, copy the save folder as usual; nothing outside it is needed.

Technical details for contributors: [compatibility](multiplayer-compatibility.md),
[synchronization](multiplayer-synchronization.md) and
[testing](multiplayer-testing.md).
