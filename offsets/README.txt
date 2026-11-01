LOOTLINE - GAME UPDATE KIT
==========================

Everything in this folder is built into Lootline (Lootline.exe / LL_Debug.exe) when you run BUILD.bat.

WHEN CS2 UPDATES
----------------
1. Get a fresh dump (cs2-dumper, or a CS2 offsets repository's zip).
2. Copy these 3 files from it into THIS folder (replace the old ones):
       offsets.json
       buttons.json
       info.json
   (only these 3 are needed - the rest of the dump can stay where it is)
3. Double-click BUILD.bat in the Lootline folder. Wait for "BUILD OK".
4. Run the new Lootline.exe (x64\Release) or LL_Debug.exe (x64\Debug).
5. In the menu: Settings -> Game update -> Check signatures.
     green  = everything is fine.
     yellow = a signature broke but the offsets dump covers it (Input / local player). Fine.
     red    = that signature broke. Find a new one (UnknownCheats thread, a pattern list), put it in
              patterns.txt (instructions at the top of that file), and do step 3 again.

WHAT THE DUMP IS FOR
--------------------
Lootline finds almost everything by signature, and by the game's own schema (never needs updating).
The dump is the backup for what a dump has: dwCSGOInput and dwLocalPlayerPawn.
It's only used when it really fits the running game, so an old dump can't break anything.

NOTE: some dumps have a wrong build number in info.json. That's fine - Lootline checks the offsets
themselves, not the number.
