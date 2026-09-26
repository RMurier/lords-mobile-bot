# Analyzing the game client (APK)

The Android APK is a Unity IL2CPP build: the real game logic (C#) is ahead-of-time compiled to
native ARM code in `lib/<abi>/libil2cpp.so`, with `assets/bin/Data/Managed/Metadata/global-metadata.dat`
carrying the type/method names needed to make sense of it. There is no bytecode to read directly -
only Il2CppDumper can turn the pair back into class/method *signatures* (never method bodies -
those stay native and need a disassembler/decompiler like Ghidra).

This gives two levels of information, cheapest first:

1. **Static dump (seconds, no decompiling)**: every class, field, and method name/signature. Enough
   to confirm a mechanic exists and find candidate functions by name, without touching Ghidra.
2. **Decompiled pseudocode (slow, one function at a time)**: what a specific function actually does.
   Needed to pin down an exact formula; expensive and occasionally hits real tooling limits (below).

## 1. Static dump: extract + Il2CppDumper

```bash
# From the APK (a plain zip):
#   lib/arm64-v8a/libil2cpp.so
#   assets/bin/Data/Managed/Metadata/global-metadata.dat

# Il2CppDumper needs a matching .NET runtime. Release builds are framework-dependent
# (net6/net7); a locally-installed newer runtime works via roll-forward:
curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 8.0 --runtime dotnet --install-dir ~/dotnet
export DOTNET_ROLL_FORWARD=LatestMajor

# https://github.com/Perfare/Il2CppDumper/releases - grab an Il2CppDumper-net7-*.zip
dotnet Il2CppDumper.dll libil2cpp.so global-metadata.dat ./output
```

Output: `dump.cs` (every class/field/method signature, ~50 MB of text for this game - `grep`/`awk`
it directly, don't try to open it in an editor), `script.json` (the same info as structured JSON,
including each method's exact RVA and native parameter types - more reliable than dump.cs's
comments when a signature matters), `stringliteral.json`.

**Always cross-check a method's real signature in `script.json` before trusting dump.cs's C#-style
listing.** Ghidra's own parameter-recovery heuristic can be wrong (see below), and the only source
of truth for "how many native params, what types" is `script.json`'s `TypeSignature`/`Signature`
fields, not what a decompiler guesses.

## 2. Decompiling a specific function with Ghidra (headless)

Needed tools: a JDK (**21+, not 17** - Ghidra 12.x refuses older ones), Ghidra itself. Both can be
fetched into a scratch dir with no root/apt needed:

```bash
curl -sL https://api.adoptium.net/v3/binary/latest/21/ga/linux/x64/jdk/hotspot/normal/eclipse -o jdk21.tar.gz
mkdir jdk21 && tar xzf jdk21.tar.gz -C jdk21 --strip-components=1

curl -sL "$(curl -sL https://api.github.com/repos/NationalSecurityAgency/ghidra/releases/latest \
  | grep -o '"browser_download_url": *"[^"]*"' | cut -d'"' -f4)" -o ghidra.zip
python3 -c "import zipfile; zipfile.ZipFile('ghidra.zip').extractall('.')"
```

**Gotchas that will otherwise cost real time** (all hit, all confirmed):

- **Zip extraction drops the executable bit.** `chmod +x` both `support/analyzeHeadless` and, less
  obviously, `Ghidra/Features/Decompiler/os/linux_x86_64/{decompile,sleigh}` - the decompiler
  silently reports every function as "failed to decompile" with an *empty* error message if this
  native helper isn't executable. That empty-message symptom is the tell.
- **`-postScript` only accepts `.py` if Ghidra was started with the PyGhidra bridge** (real CPython,
  not bundled). Ghidra 12.x dropped Jython. Simplest fix: write the script as a `.java` `GhidraScript`
  instead - headless compiles and runs those directly, no Python setup at all.
- **The compiled-script cache silently serves a stale build on a compile error.** If a script edit
  introduces a type error, headless logs `decompile_target.java:NN: error: ...` *and then keeps
  running the last successfully-compiled version* with no further warning - so the run "succeeds"
  and produces output that doesn't reflect the edit at all. Always grep the run's own log for
  `error:` after any script change; when in doubt, `rm -rf ~/.config/ghidra/<version>/osgi` to force
  a clean recompile.
- **`CreateFunctionCmd` can hang indefinitely** on some functions (its bundled constant/operand
  analysis loop never converges within any reasonable timeout - confirmed stuck past 590s on one
  ~300-instruction function while a *different*, larger function's analysis finished in seconds).
  Skip it: disassemble a bounded window with `DisassembleCommand` and build the function directly
  from the resulting instructions via `FunctionManager.createFunction`.
- **`DisassembleCommand(AddressSetView startSet, AddressSetView restrictedSet, boolean followFlow)`
  - the two AddressSetView args are easy to swap.** `startSet` is where to begin, `restrictedSet` is
  the hard boundary; passing `null` for the boundary means "no limit" even with `followFlow=false`,
  and linear disassembly will run on for tens of thousands of instructions past where you meant to
  stop.
- **Linear disassembly (`followFlow=false`) stops dead at the first byte it can't read as an
  instruction** - almost always an inline literal/constant pool the compiler placed inside the
  function body (common on ARM64). It won't skip over this on its own. Working pattern: disassemble
  from the current cursor, note where it actually stopped, skip forward a few bytes, and repeat -
  "hopping" over each data island - until enough of the function is covered.
- **A method's real IL2CPP signature can differ from what Ghidra's decompiler infers** from the
  native code alone (seen: a real `bool Method(this, method)` misread as three params, one a bogus
  16-byte struct - the decompiled body was garbage until the signature was forced). Fix: build the
  correct `Parameter` list from `script.json`'s `TypeSignature` (`this` + N declared C# params +
  trailing `method` pointer - IL2CPP's own convention for every instance method) and call
  `Function.updateFunction(..., FunctionUpdateType.DYNAMIC_STORAGE_ALL_PARAMS, ...)` before
  decompiling, rather than trusting auto-detected parameters.
- Very large functions can still exceed the decompiler's own flow-complexity limit (`Low-level
  Error: Flow exceeded maximum allowable instructions`) or its default 60s timeout - both fixable by
  raising `ifc.decompileFunction(func, seconds, monitor)`'s timeout, but some functions (a full
  screen's `OnOpen`) are large enough that even 500+ seconds and 49,000+ decompiled instructions
  don't reach the interesting part; at that point diminishing returns kick in and it's a judgment
  call whether to keep pushing.

## Findings so far

### The attribute system (`GATTR_ENUM`)

Almost every derived stat in the game - not just troop training - is one numbered slot in a single
enum (`GATTR_ENUM`, ~330 entries; `gamedata/gattr_enum.cs` has the full list, extracted from the
IL2CPP dump the same way as `gamedata/buildings.json`/`research.json`). Buildings, research, VIP,
gear, talents, etc. each contribute to these slots, which some
central resolver sums/combines into the actual number the game uses. Confirmed to exist as a formal
system (not inferred) via the IL2CPP dump. Entries relevant to this bot's existing or possible
features:

| Enum | Id | Relevance |
|---|---|---|
| `EGA_TRAINING_CAPACITY` / `_PERCENT` | 65 / 66 | Max troops trainable per order (see below) |
| `EGA_GATHERING_CAPACITY` | 73 | Likely governs gather march troop cap - not yet investigated |
| `EGA_REINFORCE_CAPACITY` | 74 | Alliance reinforcement troop cap |
| `EGA_RALLY_CAPACITY` | 75 | Rally troop cap - relevant to any future auto-rally feature |
| `EGA_MARCH_NUM` | 32 | Almost certainly what sets `max_marches` |
| `EGA_TROOP_LOAD` / `_DEBUFF` | 18 / 23 | Per-troop carry capacity - feeds gather amount math |
| `EGA_MARCH_SPEED` / `_DEBUFF` | 19 / 24 | March speed |
| `EGA_HOSPITAL_CAPACITY` | 69 | Infirmary capacity - relevant to `$heal` |
| `EGA_TRAP_CAPACITY` | 67 | Wall trap capacity |
| `EGA_MONSTERPOINT_MAX` / `_RECOVER` | 118 / 116 | Monster-hunting uses an energy-pool mechanic (max + regen rate), not a per-hunt cooldown - relevant to any future auto-Darknest/monster feature |
| `EGA_COMBAT_GROUP_CAPACITY` | 106 | Max squad/group size in a fight |

None of these beyond training capacity have been traced to an exact formula - this is a map of
what exists and where to start, from *confirmed* enum names, not a guess about what they do.

### Troop training capacity ("how many troops can I train in one order")

Confirmed the account's own owner (in-game observation) that the real per-order cap is the **same
for every troop kind and tier at a given moment**, and changes over time (29 at one point, 7226 at
another, same account) - only price/duration differ by kind/tier, not the cap.

Traced with certainty (real decompiled code, not inference):
- `Barrack` building's own capacity table is public wiki data, already in `gamedata/buildings.json`:
  level 25 = 5000, +10 per mana level. Boosted by 3 researches named "Barracks Expansion I/II/III"
  (`gamedata/research.json`), each a cumulative percentage, up to +20%/+10%/+5% at max level. Only
  Expansion II's protocol id is confirmed in this repo (`TECH_BARRACK_EXPANSION_II = 299`,
  `include/tech_research.h`) - I and III aren't yet mapped to a protocol id.
- `GATTR_ENUM.EGA_TRAINING_CAPACITY` (65) and `_PERCENT` (66) exist as formal attribute slots
  matching this mechanic (flat + percentage, same pattern as the wiki's base+research split).
- `UIBarrack_Soldier.CheckMaxTroops()` (the function that enforces the cap when you try to train)
  reads the cap from `this.m_UnitRS`'s cached max value, which is a straight copy of
  **`GUIManager.Barrack_Soldier_SliderValue`** - a field with that exact name in the compiled game
  code. Confirmed via decompiled pseudocode, not a guess.
- Every other `UIBarrack_Soldier` method checked (`SetLockValue`, `SetLockValue_T5`,
  `SetLockBtnType`) also only *reads* `Barrack_Soldier_SliderValue` - none of them compute it.

**Not yet found**: the function that actually *writes* `Barrack_Soldier_SliderValue` - i.e. the
real formula combining building capacity + research bonuses (+ possibly VIP/gear/talents, given how
many other systems can feed a `GATTR_ENUM` slot). Decompiled `OnOpen` (the function most likely to
set it up when the screen opens) up to 49,000 instructions without finding the write; it may be
later in that same function, or set by whatever code triggers opening the screen in the first
place, before `OnOpen` even runs. Left here as the next concrete step if picked back up: search for
cross-references to the `Barrack_Soldier_SliderValue` field offset (0xCF8 on `GUIManager`) across
the whole binary rather than guessing more candidate methods by name - needs a full auto-analysis
pass (hours) rather than the targeted single-function decompiles used so far.

**Practical takeaway for the bot** (already implemented, see `docs/configuration.md`'s Automatic
Training section): since the cap can't be predicted without finding that formula, and is shared
across every kind/tier, `AutoTrainTick` learns it live - starts from a modest guess, then reuses
whatever the server actually granted last time (+50% headroom) for every subsequent request,
account-wide. This needs no formula and adapts automatically as the real cap changes over time.

### The game's data tables (research and others)

The APK's `assets/Loading/Table.unity3d` (9.5 MB, 556 tables) and `String.unity3d` are plain Unity bundles: `pip install UnityPy`
reads them, every table is a `TextAsset` (u16 version, u16 record count, then fixed-size records - the layouts are the structs of
the same names in `dump.cs`, e.g. `TechDataTbl`, `TechLevelTbl`). The PC client keeps a more recent copy under
`Lords Mobile PC_Data/Download/6000/Loading/`. Strings: `StringTable` is an index of (offset, length) pairs followed by the text
blob, and `StringTable2` maps a string id (what a table stores) to the 1-based position of that text. Research uses it, see
[research.md](research.md) and `tools/extract_game_tables.py`; the same method opens any other table.
