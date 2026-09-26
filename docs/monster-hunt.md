# Monster hunt (map monsters) - what is known

Status: **implemented** (`HuntTick` in `src/protocol.c`, settings `monster.*`, docs/configuration.md); still to do: the gift-based count of the guild members' hunts. Everything here comes from one capture (2026-09-26: a level 1 monster killed in one attack, then a
level 3 Gorzilla attacked 5 times, then one more attack after "auto" used energy items from the bag) and the game's own tables/client dump.
What the account owner wants: `memory/monster-hunt-requirements` (fixed level per account, x1 attacks until the monster dies, heroes chosen from
the monster's weakness, coordinates in the guild chat in the game's clickable format when the energy runs out, hunt when the energy is full).

## The attack: `_MSG_REQUEST_SENDMONSTER` (2488), 21 bytes
```
seq u32 | zone u16 | point u8 | 01 | hero u16 x5 | level u8 | monster key u16
1c000000 | c801    | ce       | 01 | 1000 1300 0600 0400 0500 | 01 | 2700      level 1 monster
21000000 | d801    | 0d       | 01 | (same five heroes)       | 03 | 2700      level 3 Gorzilla (same key 0x27)
```
Zone/point are the monster's map cell (as for gather tiles). The five heroes were ids 16 Bombin' Goblin, 19 Elementalist, 6 Incinerator, 4 Snow
Queen, 5 Prima Donna: all mages (`Heros.bin`: `HeroType` 3, `AttackDamage` 0, `AbilityPower` > 0). The byte before the heroes (01) is constant in the
capture (unknown). An attack is one request; the owner's "x1" is that. 

## What comes back
- `_MSG_RESP_SENDMONSTER` (2489), 31 bytes: `status u8 (0) | march index u8 (8) | zone u16 | point u8 | time u64 | travel seconds u32 | N u32 | the five heroes`.
  N goes 21240 (after the level 1 attack), 16990, 12793, 8562, 4332, 101 (5 level 3 attacks: about -4230 each) and 872 after the energy items:
  **N is the hunt energy left after the attack** (hypothesis, fits everything; a level 3 attack costs about 4230). Not confirmed against the in-game number.
- `_MSG_RESP_MONSTER_INFO` (2492), 16 bytes: `time u64 | monster serial u32 | hits on that monster u8 | 00 | 00 | 0x14`. The serial is the map record's u32 below
  (client field `LastHitMonsterSerialNO`); the hit counter goes 1..6 on the Gorzilla.
- `_MSG_RESP_MONSTERRETURN` (2490): the heroes are back (about 8 s later); `_MSG_RESP_MONSTERHOME` (2491): the march is over (1 byte, the march index).
- `_MSG_RESP_MONSTERREPORTINFO` (3422) is the battle report (monster key, level, damage...), `_MSG_RESP_LIVEMONSTERREPLAYMETA` (3460) the replay.

## The monsters on the map (`_MSG_RESP_UPDATE_MAPINFO_PLUS`, 2220)
A monster is a record `zone u16 | point u8 | 0x0a | level u8 | key u16 | serial u32 | float 100.0 ...`. After a hit the record carries the monster's
remaining health as a float in percent (92.3, 83.3, 73.5, 62.2, 50.7, 38.8 after the six hits on the Gorzilla): the bot can follow the fight from it.
Kind 0x0a is a map monster. Key 0x27 = Gorzilla (levels 1 to 5 share the key).

## Energy items: `_MSG_REQUEST_SMARTUSE_RESOURCE` (1429) / `_MSG_RESP_SMARTUSE_RESOURCE` (1430)
Request: `seq u32 | count u16 | count x (item u16, quantity u16)`; here items 0x048b x2 and 0x048a x1 (energy items). The answer lists what is left
(0x048b: 58, 0x048a: 222) after a block of 40 bytes not decoded. The next attack followed at once.

## The monsters' weaknesses (solved, from the game's own table)
`Monster.bin` row (57 bytes): `key u16 | ... kind u8 at 4 | name string id u16 at 5 | attack type string id u16 at 11 | defence string id u16 at 13 | ...`.
The strings say "Physical" / "Magic" / "Physical and Magic" / "Direct DMG" (attack) and "High PDEF" / "High MDEF" / "Physical and Magic" (defence). Gorzilla
(key 39, "Gawrilla" in English) is "Physical / High PDEF", which is what the game shows. `gamedata/game_monsters.json` has the 138 monsters (name EN/FR, attack, defence).
**Rule:** defence "High PDEF" -> send magic heroes; "High MDEF" -> physical heroes; "Physical and Magic" (balanced) -> no advantage, the highest levels.

## The heroes
`docs/heroes.md` is the grid title <-> id (64 heroes, with class and damage type; `gamedata/game_heroes.json`). What the account owns is in `_MSG_RESP_HEROSAVE`
(1201): `4 bytes, u64 account, u16 count`, then 20-byte records `id u16 | level u8 (60 max) | exp u32 | rank u8 | star u8 | flags u8 | 6 zero | 4 skill levels
(60, 60, 40, 20 at the maximum)` (read on two accounts; the count matches the number of records). Owner's rule for the team: the heroes of the right damage type
first, then by level.

## Energy (decoded from two captures)
**In the login packet** `_MSG_LOGIN_ROLEINFO` (1008, 705-byte payload, the same offsets on both accounts):
```
offset 366  u32  MonsterPoint            the energy stored at the last change (872 in the second capture)
offset 370  u64  LastMonsterPointRecoverTime   server clock of that change
offset 378  u16  MonsterPointRecoverFrequency  1201 = milliseconds per point (0.83 point/s)
```
The current energy is `stored + (now - last) * 1000 / frequency`, capped at the maximum: the second capture gives 872 + 1009 s / 1.201 = 1712 at the login, and the
owner read 1744 a few seconds later; the first one 20028 + 3028 s / 1.201 = 22549. (The owner's bar said "full in 20h56m28" from 1744 to 64540: 62796 points at
0.833/s = 75400 s, the same rate.) After each attack `_MSG_RESP_SENDMONSTER` (2489) gives the stored value again (its N).

**The maximum is NOT sent by the server**: the client computes it (`GetMaxMonsterPoint()`: base + hunting gear + Monster Hunt researches, attribute 118; the cost
is reduced by attribute 117). Neither can be read from a packet: the account's maximum (64540 here) is a **setting the owner gives**.

**The cost of an attack** is what the energy drops by: a level 3 Gorzilla costs **4240** exactly (energy 4804 -> 564 after the attack, 4583 -> 343, 4352 -> 112).
Other levels: not measured (the level 1 attack of the first capture took 22549+ down to 21240).

**Using energy items** (`_MSG_REQUEST_SMARTUSE_RESOURCE`, 1429): `seq u32 | count u16 | count x (item u16, quantity u16)`, here 0x048b (x1 or x2) and 0x048a (x1)
= the energy items; the answer (1430) is `result u8 | 4 bytes | six u32 (the resources' stock) | u32 energy after the items | time u64 | u64 | count u16 | count x (item,
quantity left)`: 4804, 4583 and 4352 in the captures, each followed by the attack. The game takes the smallest cover of what the attack lacks, as for training.

## A kill and its report
`_MSG_RESP_MONSTERREPORTINFO` (3422), one per attack, before the heroes come back:
```
report id u32 | 00 | time u64 | 0d 00 | zone u16 | point u8 | KILLED u8 (0, 0, then 1 on the third attack) | u16 | monster key u16 | level u8
| monster health before u32 | monster health after u32 | ... the five heroes ...
```
(3 attacks: 5904091 -> 4083727, 4083726 -> 1902651, 1902650 -> 0). So the kill is the byte after the point (or a health of 0). Right after it, `_MSG_RESP_UPDATEITEM` (1411)
updates the bag and the map record of the monster disappears. The kill gives the guild a gift: the owner got a blue gift "Butin Gorzilla [Rare]", "Cadeau de Zyco" in
the alliance gifts (`_MSG_RESP_ALLIANCE_GIFT_OPENBOX`, 2863: header `03 count`, then per gift `id u32 | 00 | time u64 | gift type u16 | 5 zeros | giver name[13]`,
type 0x0b40 in the capture). **A guild member's kills can be counted from these gifts** (giver + gift type = monster loot), which the owner wants for tracking members' hunts.

## Still to find
- The energy cost of the other levels (measure by hunting a level 1, 2, 4, 5 once each).
- The gift type numbers of the other monsters (one per monster and rarity, to count hunts by monster).
