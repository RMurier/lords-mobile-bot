# Equipment sets and talents

What the game sends and asks when you change your equipment set or reset your talents. From one capture of the official PC client
(one account, `pktmon`, session decoded with the bot's own DES key - the server's answers are in clear, only the client's requests are
encrypted). **Nothing here is implemented in the bot**: this is what was learned so it does not have to be worked out again. Everything
is marked *confirmed* (seen in the capture) or *inferred* (not seen, deduced).

Message names and numbers are those of `include/packet_map.h`. Payloads are what follows the 4-byte packet header.

## Equipment ("stuff")

The Lord's equipment is **8 slots**. Each piece a player owns has a **serial number** (`SerialNO`, a u32 that is unique per piece, not
the item's model id): every request below talks in serial numbers, in the fixed order of the 8 slots.

| Message | Direction | What |
|---|---|---|
| `_MSG_RESP_ONLORDEQUIP_INFO` (3804) | server, at login | 8 records of 20 bytes, the pieces worn now. The serial number is the last u32 of a record; the first u16 is the item's model id. *Confirmed*: the serials worn at login (22, 23, 24, 25, 26, 28, 27, 29) are those of the first saved set |
| `_MSG_RESP_LORDEQUIP` (1417) | server, at login | every piece in the bag, one record per piece (serial number, model id, colour, enhancement, gems...; the game's `ItemLordEquip`) |
| `_MSG_RESP_ALL_LORDEQUIP_MEMORY` (6101) | server, at login | **the saved sets** (the game's "equipment memory"): `u8 ?, u8 count (10)`, then per set a **40-byte name** (NUL padded, the player types it) followed by **8 x u32 serial numbers**, so 72 bytes per set, then one flag byte per set (10 bytes at the end, `01 00 04 00 00 00 00 00 05 00` in the capture: what `_MSG_REQUEST_CHANGE_LORDEQUIP_MEMORY_FLAG` (6104) / its answer 6105 change - meaning not identified) |
| `_MSG_REQUEST_NEWBIE_QUICK_PUTON_LORDEQUIP` (3827) | client | **wear these 8 pieces at once**: `u32 seq`, then `8 x u32` serial numbers in slot order. This is what the client sent when a set was switched in the equipment window. *Confirmed* |
| `_MSG_RESP_NEWBIE_QUICK_PUTON_LORDEQUIP` (3828) | server | `u8 status (0 = ok)` then the same 8 serial numbers |

The switch is one request: there is no "take off" then "put on" per slot. In the capture the 8 serials sent (5, 20, 17, 69, 6, 15, 16, 14)
matched **no saved set exactly** (the set called "Production" has 17, 15, 14, 16 in common): the client builds the list itself, from
the set chosen and what is actually in the bag, then sends it. So to switch to a set from the bot, one would read the set's serials
from 6101 and send them, after checking each serial is still in the bag (1417) - *inferred, not tried*.

Other equipment messages exist in `packet_map.h` and were not part of this capture: `_MSG_REQUEST_PUTON_TAKEOFF_LORDEQUIP` (one
piece), `_MSG_REQUEST_LORDEQUIP_CHANGE`, `_MSG_REQUEST_CHANGE_FAVORITE_LORDEQUIP` (3829 is the favourite flag, `u64 server time, u8`).

## Talents

| Message | Direction | What |
|---|---|---|
| `_MSG_RESP_TALENTINFO` (3801) | server, at login | the current allocation, 102 bytes, one byte per talent node (the levels: e.g. `03 03 03 00 00 05 03 0f ...`). The tree's shape and each node's maximum are in the game's tables (`Talent`, `TalentLv`, `TalentTree` in `Table.unity3d`, see [apk-analysis.md](apk-analysis.md#the-games-data-tables-research-and-others)); the layout of a node is `TalentTbl` / `TalentLevelTbl` in the Il2CppDumper dump |
| `_MSG_RESP_ALL_TALENT_CACHE` (6001) | server, at login | **the saved talent layouts**: `u8 ?, u8 count (10)`, then per layout a name (up to about 12 bytes, e.g. `RssRchBui`, `mixte`, `snipe`) and the level of each node, about 142 bytes per layout. Same idea as the equipment sets |
| item **1008**, "Talent Reset" (*Réinitialisation des talents*, "Resets your Talent Points allocation") | client | **the reset**, via `_MSG_REQUEST_BUYANDUSEITEM` (1410): the game buys the item if the bag has none, then uses it |

The reset request in the capture is `u32 seq, 01 0a 00 f0 03 00 00 01 00 05 00 00 00 00 00` (`f0 03` is item 1008). The answers, in
order: `_MSG_RESP_BUYITEM` (1409, echoes the same fields, then the number bought and a u32 that looks like the gem stock left),
four `_MSG_RESP_UPDATE_RESOURCEINFO` (2015), `_MSG_RESP_USEITEM` (1407, mostly zeros, item id 1008) and `_MSG_RESP_UPDATEITEM` (1411).
The meaning of the other fields of the request (`01 0a 00 ... 01 00 05`) is not identified. **Careful: the item was bought here, that spends
gems** - anything that automates this must check the bag first.

**What was NOT in the capture**: giving talent points back after the reset. `_MSG_REQUEST_TALENT_LEVEL_ADD` (with its answer
`_MSG_RESP_TALENT_LEVEL_ADD`) is the request that raises a node, but no such request was sent, so its payload is unknown, and so is how
a saved layout (6001) is applied (one request per node, or a single one). If the bot has to manage talents, a capture of: reset, then
re-spend points node by node, then apply a saved layout, is what is missing.

## How the capture was decoded (`tools/decode_capture.py` does the last part)

```
cd C:\Users\Utilisateur\Downloads
pktmon start --capture --pkt-size 0 -f capture.etl      # PowerShell as administrator, BEFORE opening the game
# log in, do the actions, note what you did and in what order
pktmon stop
pktmon etl2pcap capture.etl -o capture.pcapng
```

The login packet is in clear and the game server flow is one TCP connection. Reassemble each direction of that flow
(`tools/extract_credentials.py` has the pcap/pcapng reader and the TCP reassembly), then cut it in packets: `u16 total size, u16 message
type, payload`. Client payloads are DES-encrypted with `ENCRYPTION_KEY` (`src/des.c`, `DecryptData()` undoes it, built as a shared
library and called from Python with `ctypes` for the analysis); server payloads are not encrypted. Names come from `include/packet_map.h`.
Item and string names come from the game's tables (`Item.bin` in `Table.unity3d`; string ids in it resolve through `StringTable2`,
see research.md).
