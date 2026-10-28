# Optional player identifiers

Players keep their original class, equipment, lighting, and spell effects. There
is no player-specific body tint or permanent outline.

**Settings → Gameplay → Player Identifiers** enables additional display cues on
your computer. It is disabled by default. Choose it from the main menu settings
before joining a game; changing it does not affect anyone else's settings.

- **Disabled:** the four original automap and hover-outline colors are reused for
  players five through eight. Party portraits and monster contribution tags use
  the original artwork. Names have no added number prefix.
- **Enabled:** players have numbers 1–8 on the automap, party portraits, monster
  contribution tags, and party name prefixes. The extra four players use darker
  shades of the original color ramps on the automap and hover outline. The small
  number labels remain readable without relying on color alone.

Outlines appear only while selecting a player, as in the original game. These
preferences change rendering only: they do not change gameplay, saved characters,
network packets, or session compatibility. The setting is saved as
`Player Identifiers=0` or `Player Identifiers=1` in the `[Game]` section of your
own `diablo.ini`. Other clients may choose differently in the same game.

All eight identifiers use the original five-sprite `monstertags.clx`; the optional
numbers are drawn at runtime, so no additional asset pack is required.
