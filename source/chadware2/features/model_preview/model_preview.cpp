#include "model_preview.h"
#include "../skinchanger/stattrak_tracker.h"

#include "model_preview_item.h"
#include "model_preview_panorama.h"

#include "../../config/config.h"
#include "../skinchanger/item_schema.h"

namespace ModelPreview
{
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        return ModelPreviewPanorama::Initialize(device, context);
    }

    void Shutdown()
    {
        ModelPreviewPanorama::Shutdown();
    }

    void ProcessMainThread()
    {
        ModelPreviewItem::ProcessPending();
        ModelPreviewPanorama::ProcessMainThread();
    }

    void TickRenderThread()
    {
        ModelPreviewPanorama::TickRenderThread();
    }

    void Submit(const Request& request)
    {
        ModelPreviewPanorama::Request(request);
    }

    void KeepAlive()
    {
        ModelPreviewPanorama::KeepAlive();
    }

    void Close()
    {
        ModelPreviewPanorama::Close();
    }

    void SetRotation(float rotationX, float rotationY)
    {
        ModelPreviewPanorama::SetRotation(rotationX, rotationY);
    }

    Frame GetFrame()
    {
        return ModelPreviewPanorama::GetFrame();
    }

    std::uint64_t DisplayedRevision()
    {
        return ModelPreviewPanorama::GetDisplayedRevision();
    }

    std::uint64_t LatestRevision()
    {
        return ModelPreviewPanorama::GetLatestRevision();
    }

    bool ReadFrame(std::vector<std::uint32_t>& pixels, std::uint32_t& width, std::uint32_t& height)
    {
        return ModelPreviewPanorama::ReadFrame(pixels, width, height);
    }

    Status GetStatus()
    {
        return ModelPreviewPanorama::GetStatus();
    }

    const char* GetStatusText()
    {
        return ModelPreviewPanorama::GetStatusText();
    }

    Request MakeWeaponRequest(std::uint16_t definitionIndex, const Config::item_skin_t& skin)
    {
        Request request{};
        request.presentation = Presentation::Weapon;
        request.definitionIndex = definitionIndex;
        request.paintKit = skin.paintKit;
        request.seed = skin.seed;
        request.wear = skin.wear;
        request.statTrakEnabled = skin.statTrakEnabled;
        request.statTrakKills = StatTrakTracker::Displayed(definitionIndex, skin);
        request.customName = skin.customName;

        if (const PaintKitInfo* paintKit = Skins::g_ItemSchema.GetPaintKit(skin.paintKit))
            request.rarity = paintKit->rarity;

        for (std::size_t i = 0; i < request.stickers.size(); ++i)
        {
            const Config::skin_sticker_t& source = skin.stickers[i];
            Sticker& destination = request.stickers[i];

            destination.enabled = source.enabled;
            destination.kit = source.kit;
            destination.wear = source.wear;
            destination.scale = source.scale;
            destination.rotation = source.rotation;
            destination.offsetX = source.offsetX;
            destination.offsetY = source.offsetY;
        }

        request.keychain.enabled = skin.keychain.enabled;
        request.keychain.id = skin.keychain.id;
        request.keychain.seed = skin.keychain.seed;
        request.keychain.sticker = skin.keychain.sticker;
        request.keychain.offsetX = skin.keychain.offsetX;
        request.keychain.offsetY = skin.keychain.offsetY;
        request.keychain.offsetZ = skin.keychain.offsetZ;

        return request;
    }

    Request MakeAgentRequest(std::uint16_t definitionIndex)
    {
        Request request{};
        request.presentation = Presentation::Player;
        request.definitionIndex = definitionIndex;

        if (const ItemDefinitionInfo* item = Skins::g_ItemSchema.GetItem(definitionIndex))
        {
            request.modelPath = item->modelName;
            request.counterTerrorist = item->isCounterTerroristAgent;
        }

        return request;
    }
}
