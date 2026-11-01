#pragma once

#include "model_preview.h"

namespace ModelPreviewWidget
{
    // Draws the currently published frame. maxWidth/maxHeight <= 0 means unbounded
    // on that axis. Passing both gives a preview that fits inside the supplied area.
    void Render(const ModelPreview::Request& request, float maxWidth = 0.0f, float maxHeight = 0.0f);

    // Back to the default side view (no rotation / zoom / pan).
    void ResetView();

    // True while the preview isn't rotated (the side view the placement view needs).
    [[nodiscard]] bool IsDefaultView();

    // Submit only after the caller's controls have produced their final state.
    void UpdateRequest(const ModelPreview::Request& request);
}
