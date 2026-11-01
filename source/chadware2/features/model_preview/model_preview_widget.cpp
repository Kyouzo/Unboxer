#include "model_preview_widget.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <imgui.h>
#include <imgui_internal.h>

#include "../../menu/localization.h"

namespace
{
    // View state of the rotating preview (shared so ResetView can put it back to the default side view).
    std::uint32_t g_lastIdentity = 0;
    float g_rotationX = 0.0f;
    float g_rotationY = 0.0f;
    float g_zoom = 1.0f;
    float g_viewCenterU = 0.5f;
    float g_viewCenterV = 0.5f;
    bool g_rotating = false;
    bool g_panning = false;
}

namespace ModelPreviewWidget
{
    void ResetView()
    {
        g_rotationX = 0.0f;
        g_rotationY = 0.0f;
        g_zoom = 1.0f;
        g_viewCenterU = 0.5f;
        g_viewCenterV = 0.5f;
        g_rotating = false;
        g_panning = false;
        ModelPreview::SetRotation(0.0f, 0.0f);
    }

    bool IsDefaultView()
    {
        return g_rotationX == 0.0f && g_rotationY == 0.0f;
    }

    void UpdateRequest(const ModelPreview::Request& request)
    {
        static bool hasSubmittedIdentity = false;
        static ModelPreview::Presentation lastSubmittedPresentation = ModelPreview::Presentation::Weapon;
        static std::uint16_t lastSubmittedDefinitionIndex = 0;

        const bool identityChanged =
            !hasSubmittedIdentity ||
            request.presentation != lastSubmittedPresentation ||
            request.definitionIndex != lastSubmittedDefinitionIndex;

        // While editing sliders/text fields, keep the existing preview alive instead of
        // rebuilding it for every intermediate value. Identity changes are different:
        // submit them immediately even if the click that selected a new model/tab is
        // still the active ImGui item, otherwise a previous presentation can remain
        // visible for one extra frame.
        if (!identityChanged && ImGui::IsAnyItemActive())
        {
            ModelPreview::KeepAlive();
            return;
        }

        ModelPreview::Submit(request);
        hasSubmittedIdentity = true;
        lastSubmittedPresentation = request.presentation;
        lastSubmittedDefinitionIndex = request.definitionIndex;
    }

    void Render(const ModelPreview::Request& request, float maxWidth, float maxHeight)
    {
        std::uint32_t& lastIdentity = g_lastIdentity;
        float& rotationX = g_rotationX;
        float& rotationY = g_rotationY;
        float& zoom = g_zoom;
        float& viewCenterU = g_viewCenterU;
        float& viewCenterV = g_viewCenterV;
        bool& rotating = g_rotating;
        bool& panning = g_panning;

        // Player previews now use MapPlayerPreviewPanel with the native loadout camera,
        // so both weapon and player presentations start from neutral rotation.
        constexpr float DEFAULT_PITCH = 0.0f;
        const float defaultRotationY = DEFAULT_PITCH;

        const std::uint32_t identity =
            (static_cast<std::uint32_t>(request.presentation) << 16) |
            static_cast<std::uint32_t>(request.definitionIndex);

        if (lastIdentity != identity)
        {
            lastIdentity = identity;
            rotationX = 0.0f;
            rotationY = defaultRotationY;
            zoom = 1.0f;
            viewCenterU = 0.5f;
            viewCenterV = 0.5f;
            rotating = false;
            panning = false;
            ModelPreview::SetRotation(rotationX, rotationY);
        }

        const ModelPreview::Frame frame = ModelPreview::GetFrame();

        if (!frame.valid)
        {
            ImGui::TextDisabled("%s", ModelPreview::GetStatusText());
            return;
        }

        const ImVec2 contentAvailable = ImGui::GetContentRegionAvail();
        float viewportWidth = maxWidth > 0.0f ? std::min(maxWidth, contentAvailable.x) : contentAvailable.x;
        float viewportHeight = maxHeight > 0.0f ? std::min(maxHeight, contentAvailable.y) : 0.0f;

        viewportWidth = std::max(viewportWidth, 1.0f);

        const float sourceAspect = static_cast<float>(frame.width) / static_cast<float>(frame.height);

        if (viewportHeight <= 0.0f)
            viewportHeight = viewportWidth / sourceAspect;

        viewportHeight = std::max(viewportHeight, 1.0f);

        // Fill the entire preview widget while preserving the source texture's aspect
        // ratio. We crop unused background in UV space instead of drawing a smaller
        // 4:3 rectangle inside the card, so zoom/pan can use the whole visible panel.
        const float viewportAspect = viewportWidth / viewportHeight;
        float baseVisibleU = 1.0f;
        float baseVisibleV = 1.0f;

        if (viewportAspect > sourceAspect)
            baseVisibleV = sourceAspect / viewportAspect;
        else if (viewportAspect < sourceAspect)
            baseVisibleU = viewportAspect / sourceAspect;

        ImGuiIO& io = ImGui::GetIO();
        ImGui::PushID(static_cast<int>(request.presentation));
        ImGui::PushID(static_cast<int>(request.definitionIndex));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);

        const ImGuiWindowFlags viewportWindowFlags = ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus;

        const bool viewportVisible = ImGui::BeginChild("##ModelPreviewViewport", ImVec2(viewportWidth, viewportHeight), ImGuiChildFlags_None, viewportWindowFlags);

        if (viewportVisible)
        {
            const ImVec2 imageMin = ImGui::GetCursorScreenPos();
            const ImVec2 imageMax(imageMin.x + viewportWidth, imageMin.y + viewportHeight);

            ImGui::InvisibleButton("##ModelPreviewInteraction", ImVec2(viewportWidth, viewportHeight),
                ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);

            const bool hovered = ImGui::IsItemHovered();
            const bool active = ImGui::IsItemActive();

            // Claim the wheel while this interaction surface is hovered. Directly
            // zeroing io.MouseWheel after handling it is too late to reliably stop a
            // scrollable ancestor from also consuming wheel-down on modern ImGui.
            // Input ownership prevents both wheel directions from leaking upward.
            ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY, 0);

            const auto getVisibleSize = [&]()
            {
                return ImVec2(baseVisibleU / zoom, baseVisibleV / zoom);
            };

            const auto clampViewCenter = [&]()
            {
                const ImVec2 visible = getVisibleSize();
                const float halfU = visible.x * 0.5f;
                const float halfV = visible.y * 0.5f;
                viewCenterU = std::clamp(viewCenterU, halfU, 1.0f - halfU);
                viewCenterV = std::clamp(viewCenterV, halfV, 1.0f - halfV);
            };

            clampViewCenter();

            if (hovered && io.MouseWheel != 0.0f)
            {
                const ImVec2 oldVisible = getVisibleSize();
                const float mouseNormX = std::clamp((io.MousePos.x - imageMin.x) / viewportWidth, 0.0f, 1.0f);
                const float mouseNormY = std::clamp((io.MousePos.y - imageMin.y) / viewportHeight, 0.0f, 1.0f);
                const float oldMinU = viewCenterU - oldVisible.x * 0.5f;
                const float oldMinV = viewCenterV - oldVisible.y * 0.5f;
                const float anchorU = oldMinU + mouseNormX * oldVisible.x;
                const float anchorV = oldMinV + mouseNormY * oldVisible.y;

                zoom *= std::pow(1.15f, io.MouseWheel);
                zoom = std::clamp(zoom, 1.0f, 3.0f);

                const ImVec2 newVisible = getVisibleSize();
                viewCenterU = anchorU - (mouseNormX - 0.5f) * newVisible.x;
                viewCenterV = anchorV - (mouseNormY - 0.5f) * newVisible.y;
                clampViewCenter();

                // Keep the wheel exclusively owned by the preview this frame as well.
                io.MouseWheel = 0.0f;
            }

            const bool shiftHeld = io.KeyShift;

            if (ImGui::IsItemClicked(ImGuiMouseButton_Middle) || (shiftHeld && ImGui::IsItemClicked(ImGuiMouseButton_Left)))
            {
                panning = true;
                rotating = false;
            }
            else if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            {
                rotating = true;
                panning = false;
            }

            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
                rotating = false;

            if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !(shiftHeld && ImGui::IsMouseDown(ImGuiMouseButton_Left)))
                panning = false;

            if (panning && active)
            {
                const ImVec2 visible = getVisibleSize();
                viewCenterU -= (io.MouseDelta.x / viewportWidth) * visible.x;
                viewCenterV -= (io.MouseDelta.y / viewportHeight) * visible.y;
                clampViewCenter();
            }
            else if (rotating && active && ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                rotationX += io.MouseDelta.x * 0.55f;
                rotationY -= io.MouseDelta.y * 0.45f;
                rotationX = std::fmod(rotationX, 360.0f);

                if (rotationX < -180.0f)
                    rotationX += 360.0f;
                else if (rotationX > 180.0f)
                    rotationX -= 360.0f;

                rotationY = std::clamp(rotationY, -80.0f, 80.0f);
                ModelPreview::SetRotation(rotationX, rotationY);
            }

            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            {
                rotationX = 0.0f;
                rotationY = defaultRotationY;
                zoom = 1.0f;
                viewCenterU = 0.5f;
                viewCenterV = 0.5f;
                rotating = false;
                panning = false;
                ModelPreview::SetRotation(rotationX, rotationY);
            }

            const ImVec2 visible = getVisibleSize();
            constexpr float PLAYER_PREVIEW_SHIFT_LEFT_PIXELS = 16.0f;
            const float playerShiftU = request.presentation == ModelPreview::Presentation::Player
                ? (PLAYER_PREVIEW_SHIFT_LEFT_PIXELS / viewportWidth) * visible.x
                : 0.0f;
            const ImVec2 uvMin(viewCenterU - visible.x * 0.5f + playerShiftU, viewCenterV - visible.y * 0.5f);
            const ImVec2 uvMax(viewCenterU + visible.x * 0.5f + playerShiftU, viewCenterV + visible.y * 0.5f);

            ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(frame.texture), imageMin, imageMax, uvMin, uvMax);

            if (hovered)
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(tr("Drag: rotate"));
                ImGui::TextUnformatted(tr("Wheel: zoom"));
                ImGui::TextUnformatted(tr("MMB / Shift + drag: pan"));
                ImGui::TextUnformatted(tr("Right click: reset"));
                ImGui::EndTooltip();
            }
        }

        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopID();
        ImGui::PopID();
    }
}
