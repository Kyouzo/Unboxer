#pragma once

#include <cstddef>
#include <cstdint>

#include "IEngineClient/IEngineClient.h"
#include "CGameEntitySystem/CGameEntitySystem.h"
#include "CEconItemSystem/CEconItemSystem.h"
#include "IFileSystem/IFileSystem.h"
#include "ILocalize/ILocalize.h"
#include "ISoundOpSystem/ISoundOpSystem.h"
#include "IResponseSystem/IResponseSystem.h"
#include "../../cs2/datatypes/cutlbuffer/cutlbuffer.h"
#include "../../cs2/entity/C_WeaponVData/C_WeaponVData.h"

class C_CSPlayerPawn;

namespace I
{
    inline CUtlBuffer* (__fastcall* ConstructUtlBuffer)(CUtlBuffer*, int, int, int) = nullptr;
    inline void* (__fastcall* DetachUtlBufferMemory)(CUtlBuffer*) = nullptr;
    inline void* (__fastcall* MemAlloc)(std::size_t) = nullptr;
    inline void(__fastcall* MemFree)(void*) = nullptr;
    inline const void* (__fastcall* FindKey)(const void*, const char*) = nullptr;
    inline const char* (__fastcall* GetKeyValueString)(const void*, const char*, char*, std::size_t) = nullptr;

    inline IEngineClient* EngineClient = nullptr;
    inline IGameResourceService* GameEntity = nullptr;
    inline void* Input = nullptr;
    inline void* InputSystem = nullptr;
    inline CSource2Client* Source2Client = nullptr;
    inline IFileSystem* FileSystem = nullptr;
    inline ILocalize* Localize = nullptr;

    // agent voice lines (agent_voice_changer.cpp): optional, each may be null
    inline ISoundOpSystem* SoundOpSystem = nullptr;
    inline IResponseRulesCache* ResponseRulesCache = nullptr;
    inline IResponseSystemManager** ResponseManager = nullptr;
    inline const ResponseRules::WheelRow* WheelBegin = nullptr;
    inline const ResponseRules::WheelRow* WheelEnd = nullptr;
    inline WeaponDefinitionRegistry** WeaponRegistry = nullptr;

    C_CSPlayerPawn* GetLocalPawn();

    class Interfaces
    {
    public:
        bool init();
    };
}
