#pragma once

namespace Patterns
{
    inline constexpr const char* InputPointer = "48 8B 0D ? ? ? ? 8B D3 E8 ? ? ? ? ? ? ? ? F2 0F 11 45";
    inline constexpr const char* GetLocalPawn = "48 83 EC 28 83 F9 FF 75 ? 48 8B 0D ? ? ? ? 48 8D 54 24 30 48 8B 01 FF 90 10 03 00 00 8B 08 48 63 C1 4C 8D 05 ? ? ? ? 33 D2 4D 8B 04 C0 4D 85 C0 74 ? 45 8B 80 BC 06 00 00";
    // Only its address is needed: the Source2Client vtable slot that holds it gets swapped (no code patch, so
    // Chadware can hook the same function at the same time).
    inline constexpr const char* FrameStageNotify = "48 89 5C 24 ? 48 89 6C 24 ? 57 48 83 EC 40 48 8B F9 33 ED";

    // Still referenced by the entity headers shared with Chadware (unused by Unboxer, not in the checker list).
    inline constexpr const char* GetBaseEntity = "4C 8D 49 10 81 FA FE 7F 00 00 ? ? 8B CA C1 F9 09 83 F9 3F ? ? 48 63 C1 4D";
    inline constexpr const char* AddKeyChainEntity = "44 88 44 24 ? 48 89 54 24 ? 48 89 4C 24 ? 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24";
    // AddNametagEntity @+0x801BA0 (2026-10-08 build; Berserk's signature, still unique): (gun, its item) - CS2's name tag
    // (weapons/models/shared/nametag/nametag_module.vmdl on the gun's "nametag" attachment; the first-person gun copies it).
    // Reads the item's custom name: the same name already on = nothing, another = replaced, none = taken off.
    inline constexpr const char* AddNametagEntity = "40 55 53 56 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B DA 48 8B F1 48 85 C9";
    inline constexpr const char* SetMeshGroupMask = "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8D 99 ? ? ? ? 48 8B 71";

    // ── skin changer (equipped Lootline items on your weapons in a match) — the same signatures as Chadware ──
    inline constexpr const char* AgentSetModel = "40 53 48 83 EC ? 48 8B D9 4C 8B C2 48 8B 0D ? ? ? ? 48 8D 54 24";
    inline constexpr const char* AgentPrecache = "40 53 55 57 48 81 EC ? ? ? ? 48 8B 01 49 8B E8 48 8B FA";
    inline constexpr const char* GloveCreatePaintKit = "48 89 5C 24 ? 56 48 83 EC ? 48 8B 01 FF 50";
    inline constexpr const char* GloveSetBodyGroup = "E8 ? ? ? ? EB 0C 48 8B CF";
    inline constexpr const char* KnifeUpdateSubclass = "4C 8B DC 53 48 81 EC ? ? ? ? 48 8B 41";
    // AnimGraphRebuild(controller, mode): rebuilds an entity's AG2 animation graph (mode 2). The first-person knife
    // keeps the previous knife's graph after a knife change (inspect / reload restart, stutter) - UC recipe. Unique on
    // the 2026-10-01 build @client.dll+0x9347D0.
    inline constexpr const char* AnimGraphRebuild = "40 55 56 48 83 EC 28 4C 89 74 24 58 48 8B F1 80 FA FF 75 04 0F B6 51 18";
    inline constexpr const char* RegenerateSkins = "48 83 EC ? E8 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 48 8B 10";
    inline constexpr const char* FindHudElement = "40 53 48 83 EC ? 48 8B 05 ? ? ? ? 48 8B D9 48 85 C0 74 ? 48 89 5C 24";
    inline constexpr const char* ClearHudWeapon = "E8 ? ? ? ? 8B F8 C6 84 24";
    inline constexpr const char* BuildModernSkin = "48 85 C9 0F 84 ? ? ? ? 48 8B C4 48 89 50 10";
    // Custom skin colours (features/unboxer/custom_color): the other two skin material builders (the in-match guns'
    // "weapons/paints/legacy/" one @+0x8004B0, the gloves' "gloves/paints/" one @+0xC66210, its pawn at +disp32 @+24) and
    // BuildModernSkin's wear variable (movaps xmm2,xmm0 / lea rdx,"g_flWearAmount" / lea rcx,[rbp+X] / call float ctor /
    // mov rdx,rax / lea rcx,[rsp+X] / call append / lea rcx,[rbp+X] / call dtor): ctor +18, append +31, dtor +43 (rel32).
    // All unique / first match on build 14192.
    inline constexpr const char* CompositeLegacyWeapon = "48 85 C9 0F 84 ? ? ? ? 55 56 57 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 45 0F B6 F9 45 0F B6 F0";
    inline constexpr const char* CompositeGloves = "40 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 4C 8B B1 ? ? ? ? 4C 8B F9 4D 85 F6";
    inline constexpr const char* CompositeLooseVariable = "0F 28 D0 48 8D 15 ? ? ? ? 48 8D 8D ? ? ? ? E8 ? ? ? ? 48 8B D0 48 8D 4C 24 ? E8 ? ? ? ? 48 8D 8D ? ? ? ? E8";
    inline constexpr const char* FindHudKeychainAddon = "48 89 5C 24 18 57 41 56 41 57 48 83 EC 20 49 8B F9 45 8B F0 45 33 C9 4C 8B FA 48 8B D9 E8";
    inline constexpr const char* RemoveHudKeychainAddon = "48 83 EC 28 48 85 C9 0F 84 ? ? ? ? 48 8B 51 10 8B 42 30 0F BA E0 0B 73 07 C1 E8";
    // Removes a client-side entity right away (UTIL_RemoveImmediate): what AddKeyChainEntity takes a gun's old charm
    // off with. Used to clear EVERY charm on a gun before ours goes on. Unique on the 2026-10-01 build @+0xA1B940.
    inline constexpr const char* RemoveKeychainModule = "48 83 EC 28 48 85 C9 74 ? 48 8B 51 10 8B 42 30 0F BA E0 0B 73 ? C1 E8 06";
    // ReleaseCompositeMaterials clears stale materials.
    inline constexpr const char* ReleaseCompositeMaterials = "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 20 44 0F B6 F2 48 8B F9 E8 ? ? ? ? 45 33 FF 48 8B F0";

    inline constexpr const char* RasteriseSvg = "48 8B C4 48 89 58 ? 48 89 70 ? 48 89 78 ? 55 41 54 41 55 41 56 41 57 48 8D 68 ? 48 81 EC ? ? ? ? 4C 8B AD";

    // Panorama: run a line of JS in the main menu (the case-opening result is handed to the UI this way).
    inline constexpr const char* UiEnginePointer = "48 89 78 ? 48 89 0D ? ? ? ?";
    inline constexpr const char* RunPanoramaScript = "4C 89 4C 24 ? 4C 89 44 24 ? 48 89 54 24 ? 55 53 56 57 41 54 41 56";
    // CEconTool_Sticker's "apply" (vfunc 9, @+0x119A6F0) reading where the sticker popup's 3D preview put the sticker:
    // slot (mov eax,[rip] at +5), offset x (movss at +0x1C), offset y (+0x35), rotation (+0x4C), wear (+0x62).
    inline constexpr const char* StickerPlacement = "40 84 F6 75 ? 8B 05 ? ? ? ? 48 8B 4C 24 ? 83 49 10 04 89 41 28 48 8B 44 24 ? F3 0F 10 05 ? ? ? ? 81 48 10 80 00 00 00 F3 0F 11 40 3C 48 8B 44 24 ? F3 0F 10 0D ? ? ? ? 81 48 10 00 01 00 00 F3 0F 11 48 40 0F 57 C9 F3 0F 10 05 ? ? ? ? 48 8B 44 24 ? 83 48 10 20 F3 0F 11 40 34 F3 0F 10 05 ? ? ? ? 0F 2F C1";
    // CEconTool_Keychain's "apply" (vfunc 9, @+0x119A310): the charm popup's placement, offset x / y / z read with
    // movss xmm, [rip] at +0, +0x27, +0x40 (globals @+0x25EF510/514/518 on the 2026-10-01 build).
    inline constexpr const char* KeychainPlacement = "F3 0F 10 05 ? ? ? ? 48 8B 0D ? ? ? ? 48 81 C1 A0 00 00 00 81 48 10 80 00 00 00 F3 0F 11 40 3C 48 8B 44 24 ? F3 0F 10 0D ? ? ? ? 81 48 10 00 01 00 00 F3 0F 11 48 40 48 8B 44 24 ? F3 0F 10 05 ? ? ? ? 81 48 10 00 02 00 00 F3 0F 11 40 44";
    // CEconTool_Patch's "apply" (vfunc 9, @+0x119A460): the patch popup's slot, mov eax, [rip] at +5 (-1 = none picked).
    inline constexpr const char* PatchSlot = "48 8D 54 24 ? 8B 05 ? ? ? ? 83 49 10 04 89 41 28 48 8B 0D ? ? ? ? 48 81 C1 A0 00 00 00";
    // ── the Add items tab's 3D preview (Chadware's model preview, unique on the 2026-10-01 build) ──
    inline constexpr const char* CreatePreviewItem = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 B8 FF 7F FF 7F FF 7F FF 7F 48 8B FA 48 39 05 ? ? ? ? 48 8B F1 75 1A";
    inline constexpr const char* PreviewBlockCtor = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B C2 33 F6 48 83 C8 02 48 8B F9 45 84 C0 48 8B DA 48 0F 44 C2 48 89 41 08 48 8D 05 ? ? ? ? 48 89 01 33 C0 48 89 41 10 48 83 C1 18 E8 ? ? ? ? 48 89 5F 30";
    inline constexpr const char* PreviewParseFromArray = "48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8D 6C 24 B9 48 81 EC B0 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 45 37 48 8B 01 48 8B DA 48 8B F9 49 63 F0 FF 50 18";
    inline constexpr const char* PreviewBlockDtor = "48 89 5C 24 10 57 48 83 EC 20 48 8D 05 ? ? ? ? 48 8B D9 48 89 01 F6 41 08 01 74 ? 48 83 C1 08 E8 ? ? ? ? EB ? 48 8B 41 08 48 83 E0 FC 48 85 C0 75 ? 48 8D 4B 18 E8 ? ? ? ? 48 83 7B 40 00 48 8D 4B 30";
    inline constexpr const char* PreviewStickerInModelPanel = "48 85 D2 0F 84 ? ? ? ? 48 89 6C 24 18 57 48 83 EC 40 48 8B CA 48 89 5C 24 50 49 8B F9";
    inline constexpr const char* PreviewPlayerConstructor = "48 89 5C 24 10 57 48 83 EC 20 48 8B F9 E8 ? ? ? ? C7 87 ? ? 00 00 00 00 80 3F 48 8D 05 ? ? ? ? 48 89 07";
    inline constexpr const char* HudPanelPointer = "48 89 35 ? ? ? ? E8 ? ? ? ? 48 85";
    inline constexpr const char* PanoramaCreateRenderTargets = "4C 8B DC 55 57 41 55 49 8D AB 68 FD FF FF 48 81 EC 80 03 00 00 48 83 B9 98 00 00 00 00 48 8B F9 0F 85 ? ? ? ?";
    inline constexpr const char* PanoramaAcquireLayerRt = "40 53 56 48 83 EC 78 48 FF 81 A0 00 00 00 48 8D B1 D0 02 00 00 48 89 7C 24 68 48 8B F9";
    // An inventory item by id (C_EconItemView*, faux ids too) @+0x83C100: what every InventoryAPI native looks items up with.
    inline constexpr const char* ItemViewById = "40 53 48 83 EC 20 48 8B D9 48 8B 0D ? ? ? ? 48 85 C9 74 ? 48 8B D3 E8 ? ? ? ? 48 85 C0 75 ? 48 B8 00 00 00 00 00 00 00 F0";
    // Misc tab (from Chadware): CCSPlayerBase_CameraServices::GetFOV (hooked: the FOV changer) and the input button
    // registration (the bunnyhop's +jump).
    inline constexpr const char* GetFov = "40 53 48 83 EC ? 48 8B D9 E8 ? ? ? ? 48 85 C0 74 ? 48 8B C8 48 83 C4 ? 5B E9";
    // C_CSWeaponBase "should draw crosshair" @+0x82A300 (bool(weapon)): no when the weapon's VData (weapon + 0x388)
    // has m_bShowCrosshair off - snipers. Hooked: the Misc tab's crosshair on snipers.
    inline constexpr const char* ShouldDrawCrosshair = "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 E8 ? ? ? ? 48 85 C0 0F 84 ? ? ? ? 48 8B 05 ? ? ? ? 80 78 58 00 74 ? B0 01";
    // From the new Chadware source (2026-10-07), checked unique on the 2026-10-07 build (client.dll):
    // the knife inspect fix - the weapon's inspect request @+0x818DF0 and the viewmodel graph's reflection of the
    // weapon's state @+0x9123B0 (knife_animation.cpp).
    // 14193 (2026-10-28): ReflectWeaponState @+0x9138F0 (its own strings: "CCS2WeaponGraphController::ReflectWeaponState")
    // pushes other registers now and its profiler line went 0xB5 -> 0xB6 - the line and the frame are wildcards.
    inline constexpr const char* KnifeInspect = "40 53 48 83 EC 40 48 8B 01 48 8B D9 FF 90 F0 0C 00 00 84 C0 75 ? 48 83 C4 40 5B C3";
    inline constexpr const char* KnifeReflect = "40 55 53 57 41 56 41 57 48 8D 6C 24 ? 48 81 EC ? ? ? ? 48 8D 05 ? ? ? ? 48 C7 45 ? ? 00 00 00 48 89 45 ? 4C 8D 45 ? 0F 10 45 ? 48 8D 05";
    // agent voice lines (agent_voice_changer.cpp): the sound event start @+0x3AE980, the radio text message
    // @+0x11A6FE0, the response rules' level init / shutdown @+0x919130 / +0x919570, the response system global
    // (+4, rel at +7), the radio wheel table (begin rel at +3, end rel at +38) and the weapon VData registry (+15, rel at +18).
    inline constexpr const char* AgentVoiceStart = "48 8B C4 55 53 56 57 41 54 48 8D A8 ? ? ? ? 48 81 EC ? ? ? ? 44 8B A5 ? ? ? ? 33 DB 4C 89 68";
    inline constexpr const char* AgentRadioText = "48 89 5C 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 81 A5 ? ? ? ? FF FF 0F FF 33 F6 8B 5A 6C 48 8B FA 48 8B 52 60 4C 8B E9";
    inline constexpr const char* AgentRulesLevelInit = "48 89 5C 24 ? 48 89 6C 24 ? 56 41 54 41 55 41 56 41 57 48 81 EC 10 01 00 00";
    inline constexpr const char* AgentRulesLevelShutdown = "48 89 5C 24 ? 55 48 83 EC 20 8B 41 ? 48 8B D9 83 E8 01 48 89 7C 24 ? 48 63 F8 BD FF FF FF FF";
    inline constexpr const char* AgentRulesDump = "48 83 EC 38 48 8B 0D ? ? ? ? 48 85 C9 74 ? 48 8B 01 FF 50 10 EB ? 33 C0 4C 8D 0D";
    inline constexpr const char* AgentWheelTable = "48 8D 3D ? ? ? ? 48 0F 45 F0 0F 1F 40 00 48 8B 17 48 8B CE FF 15 ? ? ? ? 85 C0 74 ? 48 83 C7 20 48 8D 05 ? ? ? ? 48 3B F8 75";
    inline constexpr const char* AgentWeaponRegistry = "48 8B C4 55 48 8D 68 E8 48 81 EC 10 01 00 00 48 8B 0D ? ? ? ? 4C 8D 44 24 30";
    inline constexpr const char* ButtonRegistration = "45 33 C0 48 8D 15 ? ? ? ? 48 8D 0D ? ? ? ? E8 ? ? ? ? 48 8B 05 ? ? ? ? 48 8D 0D ? ? ? ? 48 89 05 ? ? ? ? 48 8D 05 ? ? ? ? 48 89 05";
    // The chat print every chat line goes through, CS2's own "found in a container" message too. @+0x11A2760:
    // (unused, const uint8_t flags[2], int playerIndex, const char* format, ...).
    inline constexpr const char* ChatPrint = "4C 89 4C 24 20 53 56 B8 ? ? ? ? E8 ? ? ? ? 48 2B E0 48 8B 0D ? ? ? ? 41 8B D8 48 8B F2";
    // Includes the ItemPreviewPanel lookup. The global's displacement starts at +7.
    inline constexpr const char* MainMenuPanelPointer = "48 83 EC 28 48 8B 05 ? ? ? ? 48 8D 15 ? ? ? ? 48 8B 48 08 48 8B 01 FF 90 78 01 00 00 48 85 C0";

    // ── graffiti only you see (unique on the 2026-10-08 build, client.dll) ──
    // The spray wheel's "spray" @+0xD03510 (ecx the slot 0-3; InventoryAPI.ActionSpray jumps here): sends the server a
    // spray of the graffiti in that slot. Hooked: a Lootline graffiti never goes out, it's sprayed on your screen only.
    inline constexpr const char* SprayAction = "40 55 57 48 8D 6C 24 E8 48 81 EC 18 01 00 00 8B F9 E8 ? ? ? ? 48 8B 88 ? ? ? ? 48 85 C9 0F 84";
    // GetSprayApplicationError's check @+0x8D8970: (pawn, CGameTrace* out, Vector* forward out, Vector* out) - null
    // when a spray can go where you look; the trace of where you look is copied out (end +0x84, normal +0x90).
    inline constexpr const char* SprayCheck = "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 48 8D A8 ? ? ? ? 48 81 EC ? ? ? ? 4C 89 70 08 49 8B F1 4D 8B F0 48 8B DA";
    // The game's graffiti decal (CPlayerSprayDecalRenderHelper, what C_PlayerSprayDecal and the spray wheel's preview draw
    // with): SetKit @+0xD089F0 (helper, sticker kit), Place @+0xD0DB30 (helper, position, normal, -left, tint, preview;
    // every frame), Release @+0xCE40C0 (helper).
    inline constexpr const char* SpraySetKit = "48 89 5C 24 10 56 48 81 EC E0 01 00 00 8B F2 48 8B D9 3B 11 0F 84";
    inline constexpr const char* SprayPlace = "48 8B C4 48 89 48 08 55 53 56 57 41 57 48 8D A8 ? ? ? ? 48 81 EC ? ? ? ? 80 79 28 00 49 8B D9 49 8B F8 48 8B F2";
    inline constexpr const char* SprayRelease = "48 89 5C 24 08 57 48 83 EC 20 48 8B 51 20 33 FF 48 8B D9 48 85 D2 74 ? 48 8B 0D";
    // Inside Place @+0xD0E5FF: the game's globals (mov rax,[rip+disp], disp @+3) and the clock it reads, [globals + 0x30],
    // against the decal's creation time (helper +4): painted in over 1.5 s, aged from 240 s, invisible from 360 s.
    inline constexpr const char* SprayClock = "48 8B 05 ? ? ? ? 45 0F 28 ? F3 45 0F 10 ? 04 F3 0F 10 ? 30 41 0F 2F";

    // ── the kill feed (unique on the 2026-10-08 build, client.dll; from the invchanger source, FireEventClientSide re-made) ──
    // CGameEventManager::FireEventClientSide @+0x9E2BA0 (manager, event -> bool), and the event's GetName @+0x9E7E80,
    // GetString @+0x9E9580 (event, token*, default), SetString @+0xA13800 (event, token*, value),
    // GetPlayerController @+0x9E8290 (event, token*). A token = { MurmurHash2 lowercase (seed 0x31415926), -1, name }.
    inline constexpr const char* FireEventClientSide = "40 53 56 41 54 48 83 EC ? 48 8B F2 48 8D 99 ? ? ? ? 4C 8B E1";
    inline constexpr const char* EventGetName = "8B 41 14 0F BA E0 1E 73 05 48 8D 41 18 C3";
    inline constexpr const char* EventGetString = "48 83 EC 38 8B 02 48 83 C1 58 89 44 24 20 8B 42 04 89 44 24 24 48 8B 42 08 48 8D 54 24 20 48 89 44 24 28 E8 ? ? ? ? 48 83 C4 38 C3 CC CC CC 33 C9";
    inline constexpr const char* EventSetString = "48 83 EC 38 8B 02 48 83 C1 58 89 44 24 20 41 B1 1A";
    inline constexpr const char* EventGetPlayerController = "48 83 EC 38 8B 02 4C 8D 44 24 20";

    // ── the lineups (unique on the 2026-10-08 build, client.dll) ──
    // The builder of a lineup character @+0x799640 (rcx = the C_CSGO_TeamPreviewCharacterPosition): the team intro, team
    // select, wingman intro and end of match positions' vfunc 146 jumps here while a rebuild is wanted. It reads the
    // position's agent / gloves / weapon items - hooked: yours get your Lootline items first.
    // 14193 (2026-10-28): @+0x79A7C0, keeps the position in rsi instead of r14 (48 8B F1, was 4C 8B F1) - that byte is a
    // wildcard now. Its reads (m_xuid +0x618, m_agentItem +0x620, team + 0x10 / + 0x18) unchanged.
    inline constexpr const char* LineupBuild = "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ? 48 81 EC ? ? ? ? ? 8B F1 48 8B 89 ? ? ? ? E8 ? ? ? ? 45 33 ED";
    // Inside the player pawn's update (@+0xC67980, vtable slot 146) @+0xC68034: lea rcx, [pawn + glove helper] (disp @+3,
    // +0x15F8) ; call the glove routine (rel @+8, @+0xC65100: on m_bNeedToReApplyGloves it rebuilds the third-person
    // gloves - model + paint). A lineup character only runs that update while its anim graph updates.
    // Build 14192 (2026-10-22): @+0xC69414 -> routine @+0xC66210, helper still +0x15F8; the load after the second call
    // changed register (48 8B BB -> 48 8B B3), so the register is a wildcard now.
    inline constexpr const char* LineupGloveUpdate = "48 8D 8B ? ? ? ? E8 ? ? ? ? 48 8B CB E8 ? ? ? ? 48 8B ? ? ? 00 00 4C 8B";
    // A skin material owner's (entity + 0x610, CCompositeMaterialOwner) finished materials copied to another owner @+0x14E6920
    // (source, destination, release the destination's first) - what the first-person gun takes the world gun's skin with.
    // Its count field (+0x4A0) = the disp @+0xD. Lootline copies your in-match gloves' paint onto your lineup gloves.
    inline constexpr const char* LineupCompositeCopy = "48 89 5C 24 08 57 48 83 EC 20 48 8D B9 ? ? ? ? 48 8B DA 83 3F 00 7E ? 45 84 C0 74 ? 33 D2 48 8B CB E8";
    // A skin material owner "touched" @+0x14E6A10 (owner): its timestamp (+0x4D4) = now. The scene system calls it (owner
    // vtable slot 7 @+0x962A00) when the entity is drawn in a normal view; the material manager (@+0x14D5210) starts the
    // most recently touched owner's material first. Lineup characters are drawn without it -> their gloves / knife never
    // got their turn at round start. Lootline touches your lineup items every frame.
    inline constexpr const char* LineupCompositeTouch = "40 53 48 83 EC 20 48 8B D9 FF 15 ? ? ? ? F3 0F 10 8B ? ? ? ? 0F 57 D2 F2 0F 5A D0 F3 0F 5F CA F3 0F 11 8B";
    // The skin material manager's getter @+0x14D4FE0: `mov rax, [rip+disp]` @+4 (disp @+7) = the manager pointer's global.
    // Manager +0x2C allocation count, +0x30 slots (0x1B0 each, free if the int at +0 is < 0), +0x3C slot count; a request =
    // slot+0x10: +0 id (what an owner's pending list holds), +8 step, +0x88 "share an identical one", +0x100 owner,
    // +0x108 the id of the identical request it waits for. Lootline makes your lineup gloves / knife on their own when they
    // wait for your in-match ones (not drawn during the lineup).
    inline constexpr const char* LineupCompositeManager = "48 83 EC 28 48 8B 05 ? ? ? ? 48 85 C0 0F 85 ? ? ? ? 48 89 6C 24 40 48 89 74 24 48 48 89 7C 24 20 FF 15";

    // ── trade-ups (unique on the 2026-10-12 build 14190, client.dll) ──
    // CEconCraftingRecipeDefinition vfunc 1 @+0x114E9E0 (recipe, CUtlVector<CEconItem*>* items, bool partial): "do these
    // items fit this recipe" - the contract's item list, its "X / 5" count and the Ready button all ask it. For a
    // trade-up recipe it asks the collection of each item (CEconItemSetDefinition vfunc 11 @+0x11332D0, (collection,
    // quality)) whether it has knives / gloves for that item's quality when nothing in it is a grade higher (reds):
    // unique and StatTrak have them, Souvenir doesn't - CS2 keeps Souvenir reds out of contracts. In a contract of
    // only Lootline items, Lootline's Souvenir reds count as normal ones (the result is a normal knife, like CS2 gives
    // for Souvenir trade-ups).
    inline constexpr const char* TradeUpRecipeMatch = "44 88 44 24 18 48 89 54 24 10 56 41 54 48 83 EC 58 48 89 5C 24 50 45 33 E4 8B 99 ? ? ? ? 40 B6 01";
    inline constexpr const char* TradeUpCollectionRare = "89 54 24 10 53 48 83 EC 20 48 8B D9 48 8D 54 24 38 48 83 C1 60 4C 8B C1 E8 ? ? ? ? 83 F8 FF 74";

    // ── case opening (all verified unique on the 2026-09-29 build, client.dll) ──
    // InventoryAPI.UseTool(toolId, itemId) — what the Unlock button calls. @+0xFFC080. Hooked: an Unboxer case never
    // reaches Valve's servers (that's the "something went wrong"); the opening is done locally instead.
    inline constexpr const char* UseTool = "48 85 D2 0F 84 ? ? ? ? 53 41 57 48 83 EC 28 49 8B D8 4C 8B F9 48 85 DB 0F 84";
    // CCSInventoryManager::GetInstance: lea rax,[manager]; ret. @+0x83A6C0.
    inline constexpr const char* InventoryManager = "48 8D 05 ? ? ? ? C3 CC CC CC CC CC CC CC CC 8B 91 ? ? ? ? B8";
    // CEconItem factory (allocates 0x48 and sets the vtables). @+0x10D5E40.
    inline constexpr const char* CreateEconItem = "48 83 EC 28 B9 48 00 00 00 E8 ? ? ? ? 48 85";
    // GCSDK::CGCClientSharedObjectCache::CreateBaseTypeCache(classId). @+0x16236D0 (prologue was 40 53, now 40 57).
    inline constexpr const char* CreateBaseTypeCache = "40 ? 48 83 EC ? 4C 8B 49 ? 44 8B D2";

    // The Game Coordinator reply handler firing PanoramaComponent_Inventory_ItemCustomizationNotification(int32, string,
    // string) for "crate_unlock" (+0xF86120 on the 2026-10-14 build): mov r14, [UI engine] / mov r12d, [rax+38h] /
    // test r14, r14. Only where the event is made starts here: the rest (the engine's dispatch slot, the allocator, the
    // event id and vtable, the string setter) is read from the instructions that follow (unboxer.cpp ResolveNativeNotify),
    // because the 2026-10-14 update moved them all (a short je became a near one, the fields are zeroed differently).
    inline constexpr const char* CrateUnlockNotify = "4C 8B 35 ? ? ? ? 44 8B 60 38 4D 85 F6";
    // panorama.dll CUIEngine::RunFrame (vfunc 5) @+0xB6AC0: the main thread, once a frame. The engine's event dispatch
    // (vfunc 47) calls tier0 ThreadInMainThread() and silently deletes events sent from any other thread — so the
    // case result has to be sent from here. Only its address is needed: the engine's vtable slot gets swapped.
    inline constexpr const char* UiEngineRunFrame = "48 89 5C 24 ? 48 89 6C 24 ? 56 57 41 54 41 56 41 57 48 81 EC ? ? ? ? 45 33 F6 48 8B F1 44 89 B1 ? ? ? ? 0F 31";
    // Every signature by name. offsets\patterns.txt replaces them by these names, and Settings -> Game Update checks
    // them after a CS2 update. Add new signatures here too.
    struct Entry
    {
        const char* name;
        const char* module;
        const char* pattern;
        const char* usedFor;        // what stops working when it's broken (shown in the menu)
    };

    inline constexpr Entry All[] = {
        { "FrameStageNotify",            "client.dll",         FrameStageNotify,            "everything" },
        { "UseTool",                     "client.dll",         UseTool,                     "case opening" },
        { "InventoryManager",            "client.dll",         InventoryManager,            "case opening" },
        { "CreateEconItem",              "client.dll",         CreateEconItem,              "case opening" },
        { "CreateBaseTypeCache",         "client.dll",         CreateBaseTypeCache,         "case opening" },
        { "CrateUnlockNotify",           "client.dll",         CrateUnlockNotify,           "case opening" },
        { "UiEngineRunFrame",            "panorama.dll",       UiEngineRunFrame,            "case opening" },
        { "UiEnginePointer",             "client.dll",         UiEnginePointer,             "case opening" },
        { "MainMenuPanelPointer",        "client.dll",         MainMenuPanelPointer,        "case opening" },
        { "RunPanoramaScript",           "panorama.dll",       RunPanoramaScript,           "case opening" },
        { "ChatPrint",                   "client.dll",         ChatPrint,                   "drop message in chat" },
        { "StickerPlacement",            "client.dll",         StickerPlacement,            "stickers on Lootline guns" },
        { "ItemViewById",                "client.dll",         ItemViewById,                "stickers on Lootline guns" },
        { "KeychainPlacement",           "client.dll",         KeychainPlacement,           "charms on Lootline guns" },
        { "PatchSlot",                   "client.dll",         PatchSlot,                   "patches on Lootline agents" },
        { "CreatePreviewItem",           "client.dll",         CreatePreviewItem,           "3D preview" },
        { "PreviewBlockCtor",            "client.dll",         PreviewBlockCtor,            "3D preview" },
        { "PreviewParseFromArray",       "client.dll",         PreviewParseFromArray,       "3D preview" },
        { "PreviewBlockDtor",            "client.dll",         PreviewBlockDtor,            "3D preview" },
        { "PreviewStickerInModelPanel",  "client.dll",         PreviewStickerInModelPanel,  "3D preview" },
        { "PreviewPlayerConstructor",    "client.dll",         PreviewPlayerConstructor,    "3D preview" },
        { "HudPanelPointer",             "client.dll",         HudPanelPointer,             "3D preview" },
        { "PanoramaCreateRenderTargets", "panorama.dll",       PanoramaCreateRenderTargets, "3D preview" },
        { "PanoramaAcquireLayerRt",      "panorama.dll",       PanoramaAcquireLayerRt,      "3D preview" },
        { "GetLocalPawn",                "client.dll",         GetLocalPawn,                "everything" },
        { "GetBaseEntity",               "client.dll",         GetBaseEntity,               "skins in matches" },
        { "RegenerateSkins",             "client.dll",         RegenerateSkins,             "skins in matches" },
        { "BuildModernSkin",             "client.dll",         BuildModernSkin,             "skins in matches" },
        { "ReleaseCompositeMaterials",   "client.dll",         ReleaseCompositeMaterials,   "skins in matches" },
        { "CompositeLegacyWeapon",       "client.dll",         CompositeLegacyWeapon,       "custom skin colours" },
        { "CompositeGloves",             "client.dll",         CompositeGloves,             "custom skin colours" },
        { "CompositeLooseVariable",      "client.dll",         CompositeLooseVariable,      "custom skin colours" },
        { "KnifeUpdateSubclass",         "client.dll",         KnifeUpdateSubclass,         "knives in matches" },
        { "AnimGraphRebuild",            "client.dll",         AnimGraphRebuild,            "knives in matches" },
        { "GloveCreatePaintKit",         "client.dll",         GloveCreatePaintKit,         "gloves in matches" },
        { "GloveSetBodyGroup",           "client.dll",         GloveSetBodyGroup,           "gloves in matches" },
        { "SetMeshGroupMask",            "client.dll",         SetMeshGroupMask,            "knives / gloves in matches" },
        { "AgentSetModel",               "client.dll",         AgentSetModel,               "knives / gloves in matches" },
        { "AgentPrecache",               "resourcesystem.dll", AgentPrecache,               "knives / gloves in matches" },
        { "AddKeyChainEntity",           "client.dll",         AddKeyChainEntity,           "charms" },
        { "AddNametagEntity",            "client.dll",         AddNametagEntity,            "name tags on Lootline guns" },
        { "FindHudKeychainAddon",        "client.dll",         FindHudKeychainAddon,        "charms" },
        { "RemoveHudKeychainAddon",      "client.dll",         RemoveHudKeychainAddon,      "charms" },
        { "RemoveKeychainModule",        "client.dll",         RemoveKeychainModule,        "charms" },
        { "FindHudElement",              "client.dll",         FindHudElement,              "HUD weapon icons" },
        { "ClearHudWeapon",              "client.dll",         ClearHudWeapon,              "HUD weapon icons" },
        { "InputPointer",                "client.dll",         InputPointer,                "menu mouse" },
        { "GetFov",                      "client.dll",         GetFov,                      "FOV changer" },
        { "ShouldDrawCrosshair",         "client.dll",         ShouldDrawCrosshair,         "crosshair on snipers" },
        { "ButtonRegistration",          "client.dll",         ButtonRegistration,          "bunnyhop" },
        { "RasteriseSvg",                "panorama.dll",       RasteriseSvg,                "menu images" },
        { "KnifeInspect",                "client.dll",         KnifeInspect,                "knife inspect fix" },
        { "KnifeReflect",                "client.dll",         KnifeReflect,                "knife inspect fix" },
        { "AgentVoiceStart",             "client.dll",         AgentVoiceStart,             "agent voice lines" },
        { "AgentRadioText",              "client.dll",         AgentRadioText,              "agent voice lines (radio)" },
        { "AgentRulesLevelInit",         "client.dll",         AgentRulesLevelInit,         "agent voice lines" },
        { "AgentRulesLevelShutdown",     "client.dll",         AgentRulesLevelShutdown,     "agent voice lines" },
        { "AgentRulesDump",              "client.dll",         AgentRulesDump,              "agent voice lines" },
        { "AgentWheelTable",             "client.dll",         AgentWheelTable,             "agent voice lines (radio)" },
        { "AgentWeaponRegistry",         "client.dll",         AgentWeaponRegistry,         "agent voice lines (radio)" },
        { "SprayAction",                 "client.dll",         SprayAction,                 "Lootline graffiti (spraying)" },
        { "SprayCheck",                  "client.dll",         SprayCheck,                  "Lootline graffiti (spraying)" },
        { "SpraySetKit",                 "client.dll",         SpraySetKit,                 "Lootline graffiti (spraying)" },
        { "SprayPlace",                  "client.dll",         SprayPlace,                  "Lootline graffiti (spraying)" },
        { "SprayRelease",                "client.dll",         SprayRelease,                "Lootline graffiti (spraying)" },
        { "SprayClock",                  "client.dll",         SprayClock,                  "Lootline graffiti (spraying)" },
        { "FireEventClientSide",         "client.dll",         FireEventClientSide,         "knife icon in the kill feed" },
        { "EventGetName",                "client.dll",         EventGetName,                "knife icon in the kill feed" },
        { "EventGetString",              "client.dll",         EventGetString,              "knife icon in the kill feed" },
        { "EventSetString",              "client.dll",         EventSetString,              "knife icon in the kill feed" },
        { "EventGetPlayerController",    "client.dll",         EventGetPlayerController,    "knife icon in the kill feed" },
        { "LineupBuild",                 "client.dll",         LineupBuild,                 "Lootline items in the start / end lineups" },
        { "LineupGloveUpdate",           "client.dll",         LineupGloveUpdate,           "Lootline gloves in the start / end lineups" },
        { "LineupCompositeCopy",         "client.dll",         LineupCompositeCopy,         "Lootline gloves in the start / end lineups" },
        { "LineupCompositeTouch",        "client.dll",         LineupCompositeTouch,        "Lootline gloves in the start / end lineups" },
        { "LineupCompositeManager",      "client.dll",         LineupCompositeManager,      "Lootline gloves in the start / end lineups" },
        { "TradeUpRecipeMatch",          "client.dll",         TradeUpRecipeMatch,          "Lootline Souvenir reds in trade-ups" },
        { "TradeUpCollectionRare",       "client.dll",         TradeUpCollectionRare,       "Lootline Souvenir reds in trade-ups" },
    };
}
