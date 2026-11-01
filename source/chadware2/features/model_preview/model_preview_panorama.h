#pragma once

#include "model_preview.h"

#include <cstdint>
#include <vector>

namespace ModelPreviewPanorama
{
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();

    void ProcessMainThread();
    void TickRenderThread();

    void Request(const ModelPreview::Request& request);
    void SetRotation(float rotationX, float rotationY);
    void KeepAlive();
    void Close();

    [[nodiscard]] ModelPreview::Frame GetFrame();

    // Item revision whose frame is on screen right now (0 = none) / newest revision submitted.
    [[nodiscard]] std::uint64_t GetDisplayedRevision();
    [[nodiscard]] std::uint64_t GetLatestRevision();

    // Render thread only: copy the displayed frame to the CPU as 0xAABBGGRR pixels.
    bool ReadFrame(std::vector<std::uint32_t>& pixels, std::uint32_t& width, std::uint32_t& height);
    [[nodiscard]] ModelPreview::Status GetStatus();
    [[nodiscard]] const char* GetStatusText();
}
