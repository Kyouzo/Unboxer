#include "skin_images.h"
#include "../../../../patterns.h"

#include <Windows.h>
#include <dxgiformat.h>
#include <dxgitype.h>
#include <dcommon.h>
#include <wincodec.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../interfaces/interfaces.h"
#include "../../../utils/memory/patternscan/patternscan.h"
#include "../../../utils/memory/vfunc/vfunc.h"
#include "../../../../cs2/datatypes/cutlbuffer/cutlbuffer.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
constexpr std::uint64_t MAX_IMAGE_FILE_SIZE = 32ULL * 1024ULL * 1024ULL;
    constexpr unsigned int MAX_TEXTURE_DIMENSION = 4096;
    constexpr float SVG_RASTER_SCALE = 2.0f;
    constexpr std::size_t STICKER_LOAD_BUDGET_PER_FRAME = 8;

    // New pictures are read and decoded when a card first asks for one. Opening a tab with dozens of new ones did all of
    // that in one frame and the game froze (Upgrader, Add items). Now at most ~4 ms of it a frame (always one at least);
    // the rest return null for now - nothing is marked failed - and come in over the next frames.
    constexpr double LOAD_BUDGET_MS = 4.0;
    double g_loadSpentMs = 0.0;     // render thread, this frame
    int g_loadsThisFrame = 0;

    bool LoadAllowed()
    {
        return g_loadsThisFrame == 0 || g_loadSpentMs < LOAD_BUDGET_MS;
    }

    // Counts one load's time against this frame's budget.
    struct LoadTimer
    {
        LARGE_INTEGER start{};

        LoadTimer()
        {
            QueryPerformanceCounter(&start);
        }

        ~LoadTimer()
        {
            LARGE_INTEGER end{}, frequency{};
            QueryPerformanceCounter(&end);
            QueryPerformanceFrequency(&frequency);
            g_loadSpentMs += static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
            ++g_loadsThisFrame;
        }

        LoadTimer(const LoadTimer&) = delete;
        LoadTimer& operator=(const LoadTimer&) = delete;
    };

    constexpr std::uint32_t RESOURCE_BLOCK_DATA = 0x41544144;
    constexpr std::uint32_t VTEX_EXTRA_COMPRESSED_MIP_SIZE = 4;

    constexpr std::uint8_t VTEX_DXT1 = 1;
    constexpr std::uint8_t VTEX_DXT5 = 2;
    constexpr std::uint8_t VTEX_RGBA8888 = 4;
    constexpr std::uint8_t VTEX_JPEG_RGBA8888 = 15;
    constexpr std::uint8_t VTEX_PNG_RGBA8888 = 16;
    constexpr std::uint8_t VTEX_JPEG_DXT5 = 17;
    constexpr std::uint8_t VTEX_PNG_DXT5 = 18;
    constexpr std::uint8_t VTEX_BC6H = 19;
    constexpr std::uint8_t VTEX_BC7 = 20;
    constexpr std::uint8_t VTEX_ATI2N = 21;
    constexpr std::uint8_t VTEX_ATI1N = 27;
    constexpr std::uint8_t VTEX_BGRA8888 = 28;
    constexpr std::uint8_t VTEX_WEBP_RGBA8888 = 29;
    constexpr std::uint8_t VTEX_WEBP_DXT5 = 30;

    using RasteriseSvgFn = bool(__fastcall*)(
        const char* svgData,
        unsigned int svgDataLength,
        CUtlBuffer* outputBuffer,
        unsigned int* textureWidth,
        unsigned int* textureHeight,
        float scale,
        void* unknown
        );

    struct VtexInfo
    {
        std::size_t dataOffset = 0;
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        std::uint16_t depth = 0;
        std::uint8_t format = 0;
        std::uint8_t mipLevels = 0;
        bool compressedMips = false;
        bool hasMipSizeTable = false;
        std::vector<std::uint32_t> mipSizes;
    };

    ID3D11Device* g_device = nullptr;
    RasteriseSvgFn g_rasteriseSvg = nullptr;
    IWICImagingFactory* g_wicFactory = nullptr;
    bool g_comInitialized = false;

    std::unordered_map<std::string, SkinImages::Image> g_images;
    std::unordered_set<std::string> g_failedImages;

    std::unordered_map<std::string, SkinImages::Image> g_stickerImages;
    std::unordered_set<std::string> g_failedStickerImages;
    std::unordered_map<int, std::string> g_stickerResolvedPaths;

    bool ReadGameFile(const std::string& path, std::vector<std::uint8_t>& output)
    {
        output.clear();

        if (!I::FileSystem || path.empty())
            return false;

        void* handle = I::FileSystem->Open(path.c_str());

        if (!handle)
            return false;

        const std::uint64_t size = I::FileSystem->Size(handle);

        if (size == 0 || size > MAX_IMAGE_FILE_SIZE)
        {
            I::FileSystem->Close(handle);
            return false;
        }

        output.resize(static_cast<std::size_t>(size));

        const int bytesRead = I::FileSystem->Read(output.data(), static_cast<int>(size), handle);
        I::FileSystem->Close(handle);

        if (bytesRead != static_cast<int>(size))
        {
            output.clear();
            return false;
        }

        return true;
    }

    bool ExtractSvgData(const std::vector<std::uint8_t>& fileData, std::string& svgData)
    {
        svgData.clear();

        if (fileData.empty())
            return false;

        const std::string compiledData(reinterpret_cast<const char*>(fileData.data()), fileData.size());

        const std::size_t svgStart = compiledData.find("<svg");

        if (svgStart == std::string::npos)
            return false;

        constexpr const char* SVG_END = "</svg>";

        const std::size_t svgEnd = compiledData.find(SVG_END, svgStart);

        if (svgEnd == std::string::npos)
            return false;

        const std::size_t finalEnd = svgEnd + std::char_traits<char>::length(SVG_END);

        if (finalEnd <= svgStart)
            return false;

        svgData.assign(compiledData.data() + svgStart, finalEnd - svgStart);
        return !svgData.empty();
    }

    bool CreateTexture(const void* rgba, unsigned int width, unsigned int height, SkinImages::Image& output)
    {
        output = {};

        if (!g_device || !rgba)
            return false;

        if (width == 0 || height == 0 || width > MAX_TEXTURE_DIMENSION || height > MAX_TEXTURE_DIMENSION)
            return false;

        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = width;
        textureDesc.Height = height;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_IMMUTABLE;
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initialData{};
        initialData.pSysMem = rgba;
        initialData.SysMemPitch = width * 4;

        ID3D11Texture2D* texture = nullptr;

        HRESULT result = g_device->CreateTexture2D(&textureDesc, &initialData, &texture);

        if (FAILED(result) || !texture)
            return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
        viewDesc.Format = textureDesc.Format;
        viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D.MostDetailedMip = 0;
        viewDesc.Texture2D.MipLevels = 1;

        ID3D11ShaderResourceView* view = nullptr;

        result = g_device->CreateShaderResourceView(texture, &viewDesc, &view);
        texture->Release();

        if (FAILED(result) || !view)
            return false;

        output.texture = view;
        output.width = width;
        output.height = height;

        return true;
    }

    std::string GetEquipmentIconName(const ItemDefinitionInfo& item)
    {
        std::string name = !item.itemName.empty() ? item.itemName : item.simpleWeaponName;

        if (name.empty())
            return {};

        constexpr const char* WEAPON_PREFIX = "weapon_";
        constexpr std::size_t WEAPON_PREFIX_LENGTH = 7;

        if (name.compare(0, WEAPON_PREFIX_LENGTH, WEAPON_PREFIX) == 0)
            name.erase(0, WEAPON_PREFIX_LENGTH);

        return name;
    }

    std::string BuildEquipmentIconPath(const ItemDefinitionInfo& item)
    {
        const std::string name = GetEquipmentIconName(item);

        if (name.empty())
            return {};

        std::string path = "panorama/images/icons/equipment/";
        path.reserve(path.size() + name.size() + 7);
        path += name;
        path += ".vsvg_c";

        return path;
    }

    std::string BuildBaseWeaponImagePath(const ItemDefinitionInfo& item)
    {
        if (!item.isWeapon)
            return {};

        const std::string& name = !item.itemName.empty() ? item.itemName : item.simpleWeaponName;

        if (name.empty())
            return {};

        std::string path = "panorama/images/econ/weapons/base_weapons/";
        path.reserve(path.size() + name.size() + 12);
        path += name;
        path += "_png.vtex_c";
        return path;
    }

    std::string BuildPaintKitImagePath(const ItemDefinitionInfo& item, const PaintKitInfo& paintKit)
    {
        if (item.itemName.empty() || paintKit.name.empty() || paintKit.id <= 0)
            return {};

        std::string path = "panorama/images/econ/default_generated/";
        path.reserve(path.size() + item.itemName.size() + paintKit.name.size() + 20);
        path += item.itemName;
        path += "_";
        path += paintKit.name;
        path += "_light_png.vtex_c";

        return path;
    }

    std::string BuildAgentImagePath(const ItemDefinitionInfo& item)
    {
        if (!item.isAgent || item.itemName.empty())
            return {};

        std::string path = "panorama/images/econ/characters/";
        path.reserve(path.size() + item.itemName.size() + 12);
        path += item.itemName;
        path += "_png.vtex_c";
        return path;
    }

    int GetKnifePreviewPaintKitId(std::uint16_t definitionIndex)
    {
        switch (definitionIndex)
        {
        case 500:  // Bayonet
        case 503:  // Classic Knife
        case 505:  // Flip Knife
        case 506:  // Gut Knife
        case 507:  // Karambit
        case 508:  // M9 Bayonet
        case 509:  // Huntsman Knife
        case 512:  // Falchion Knife
        case 514:  // Bowie Knife
        case 515:  // Butterfly Knife
        case 516:  // Shadow Daggers
        case 517:  // Paracord Knife
        case 518:  // Survival Knife
        case 519:  // Ursus Knife
        case 520:  // Navaja Knife
        case 521:  // Nomad Knife
        case 522:  // Stiletto Knife
        case 523:  // Talon Knife
        case 525:  // Skeleton Knife
        case 526:  // Kukri Knife
            return 38; // Fade
        default:
            return 0;
        }
    }

    int GetGlovePreviewPaintKitId(std::uint16_t definitionIndex)
    {
        switch (definitionIndex)
        {
        case 4725: return 10085; // Broken Fang Gloves | Jade
        case 5027: return 10006; // Bloodhound Gloves | Charred
        case 5030: return 10018; // Sport Gloves | Superconductor
        case 5031: return 10013; // Driver Gloves | Lunar Weave
        case 5032: return 10021; // Hand Wraps | Slaughter
        case 5033: return 10026; // Moto Gloves | Spearmint
        case 5034: return 10033; // Specialist Gloves | Crimson Kimono
        case 5035: return 10057; // Hydra Gloves | Emerald
        default: return 0;
        }
    }

    std::string NormalizeStickerMaterial(std::string material)
    {
        std::replace(material.begin(), material.end(), '\\', '/');

        while (!material.empty() && material.front() == '/')
            material.erase(material.begin());

        constexpr const char* prefixes[] = {
            "panorama/images/econ/stickers/",
            "materials/models/weapons/customization/stickers/",
            "models/weapons/customization/stickers/",
            "econ/stickers/"
        };

        for (const char* prefix : prefixes)
        {
            const std::size_t length = std::strlen(prefix);

            if (material.compare(0, length, prefix) == 0)
            {
                material.erase(0, length);
                break;
            }
        }

        constexpr const char* extensions[] = {
            ".vmat_c", ".vmat", ".vmt", ".vtf"
        };

        for (const char* extension : extensions)
        {
            const std::size_t length = std::strlen(extension);

            if (material.size() >= length &&
                material.compare(material.size() - length, length, extension) == 0)
            {
                material.erase(material.size() - length);
                break;
            }
        }

        return material;
    }

    std::string BuildStickerImagePathFromMaterial(const std::string& material, bool highResolution)
    {
        if (material.empty())
            return {};

        std::string path = "panorama/images/econ/stickers/";
        path.reserve(path.size() + material.size() + 24);
        path += material;
        path += highResolution ? "_1355_37_png.vtex_c" : "_png.vtex_c";
        return path;
    }

    const std::string& ResolveStickerImagePath(const StickerKitInfo& sticker)
    {
        static const std::string empty;

        if (sticker.id <= 0 || sticker.material.empty())
            return empty;

        if (const auto it = g_stickerResolvedPaths.find(sticker.id);
            it != g_stickerResolvedPaths.end())
        {
            return it->second;
        }

        const std::string material = NormalizeStickerMaterial(sticker.material);

        if (material.empty())
        {
            const auto [it, inserted] = g_stickerResolvedPaths.emplace(sticker.id, std::string{});
            return it->second;
        }

        std::string path = BuildStickerImagePathFromMaterial(material, false);

        // Current CS2 ships the normal sticker inventory icons under this path.
        // A minority of older/special assets are only useful in the larger
        // 1355x37 inventory variant, so retain it as a transparent fallback.
        if (I::FileSystem && !I::FileSystem->Exists(path.c_str(), "GAME"))
        {
            std::string highResolution =
                BuildStickerImagePathFromMaterial(material, true);

            if (I::FileSystem->Exists(highResolution.c_str(), "GAME"))
                path = std::move(highResolution);
        }

        const auto [it, inserted] =
            g_stickerResolvedPaths.emplace(sticker.id, std::move(path));

        return it->second;
    }

    std::uint16_t ReadU16(const std::vector<std::uint8_t>& data, std::size_t offset)
    {
        if (offset > data.size() || sizeof(std::uint16_t) > data.size() - offset)
            return 0;

        std::uint16_t value = 0;
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return value;
    }

    std::uint32_t ReadU32(const std::vector<std::uint8_t>& data, std::size_t offset)
    {
        if (offset > data.size() || sizeof(std::uint32_t) > data.size() - offset)
            return 0;

        std::uint32_t value = 0;
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return value;
    }

    int DecompressLz4(const std::uint8_t* source, std::size_t sourceSize, std::uint8_t* destination, std::size_t destinationSize)
    {
        if (!source || !destination)
            return -1;

        const std::uint8_t* input = source;
        const std::uint8_t* inputEnd = source + sourceSize;

        std::uint8_t* output = destination;
        std::uint8_t* outputEnd = destination + destinationSize;

        while (input < inputEnd)
        {
            const std::uint8_t token = *input++;
            std::size_t literalLength = token >> 4;

            if (literalLength == 15)
            {
                std::uint8_t value = 255;

                while (value == 255)
                {
                    if (input >= inputEnd)
                        return -1;

                    value = *input++;
                    literalLength += value;
                }
            }

            if (literalLength > static_cast<std::size_t>(inputEnd - input) || literalLength > static_cast<std::size_t>(outputEnd - output))
                return -1;

            if (literalLength > 0)
            {
                std::memcpy(output, input, literalLength);
                input += literalLength;
                output += literalLength;
            }

            if (input >= inputEnd)
                return static_cast<int>(output - destination);

            if (inputEnd - input < 2)
                return -1;

            const std::uint16_t matchOffset = static_cast<std::uint16_t>(input[0] | (static_cast<std::uint16_t>(input[1]) << 8));
            input += 2;

            if (matchOffset == 0 || matchOffset > static_cast<std::size_t>(output - destination))
                return -1;

            std::size_t matchLength = token & 0x0F;

            if (matchLength == 15)
            {
                std::uint8_t value = 255;

                while (value == 255)
                {
                    if (input >= inputEnd)
                        return -1;

                    value = *input++;
                    matchLength += value;
                }
            }

            matchLength += 4;

            if (matchLength > static_cast<std::size_t>(outputEnd - output))
                return -1;

            std::uint8_t* match = output - matchOffset;

            // LZ4 matches may overlap their own output.
            while (matchLength--)
                *output++ = *match++;
        }

        return static_cast<int>(output - destination);
    }

    bool IsEncodedVtexFormat(std::uint8_t format)
    {
        return
            format == VTEX_JPEG_RGBA8888 ||
            format == VTEX_PNG_RGBA8888 ||
            format == VTEX_JPEG_DXT5 ||
            format == VTEX_PNG_DXT5 ||
            format == VTEX_WEBP_RGBA8888 ||
            format == VTEX_WEBP_DXT5;
    }

    bool IsBlockCompressedVtexFormat(std::uint8_t format)
    {
        return
            format == VTEX_DXT1 ||
            format == VTEX_DXT5 ||
            format == VTEX_BC6H ||
            format == VTEX_BC7 ||
            format == VTEX_ATI2N ||
            format == VTEX_ATI1N;
    }

    std::size_t GetVtexMipDataSize(
        std::uint8_t format,
        unsigned int width,
        unsigned int height,
        unsigned int depth)
    {
        width = (std::max)(1u, width);
        height = (std::max)(1u, height);
        depth = (std::max)(1u, depth);

        if (format == VTEX_RGBA8888 || format == VTEX_BGRA8888)
        {
            return
                static_cast<std::size_t>(width) *
                static_cast<std::size_t>(height) *
                static_cast<std::size_t>(depth) * 4u;
        }

        unsigned int blockBytes = 0;

        if (format == VTEX_DXT1 || format == VTEX_ATI1N)
            blockBytes = 8;
        else if (
            format == VTEX_DXT5 ||
            format == VTEX_BC6H ||
            format == VTEX_BC7 ||
            format == VTEX_ATI2N)
        {
            blockBytes = 16;
        }

        if (blockBytes == 0)
            return 0;

        const unsigned int blocksWide = (std::max)(1u, (width + 3u) / 4u);
        const unsigned int blocksHigh = (std::max)(1u, (height + 3u) / 4u);

        return
            static_cast<std::size_t>(blocksWide) *
            static_cast<std::size_t>(blocksHigh) *
            static_cast<std::size_t>(depth) *
            static_cast<std::size_t>(blockBytes);
    }

    DXGI_FORMAT GetVtexDxgiFormat(std::uint8_t format)
    {
        switch (format)
        {
        case VTEX_DXT1:
            return DXGI_FORMAT_BC1_UNORM;
        case VTEX_DXT5:
            return DXGI_FORMAT_BC3_UNORM;
        case VTEX_RGBA8888:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case VTEX_BC6H:
            return DXGI_FORMAT_BC6H_UF16;
        case VTEX_BC7:
            return DXGI_FORMAT_BC7_UNORM;
        case VTEX_ATI2N:
            return DXGI_FORMAT_BC5_UNORM;
        case VTEX_ATI1N:
            return DXGI_FORMAT_BC4_UNORM;
        case VTEX_BGRA8888:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        default:
            return DXGI_FORMAT_UNKNOWN;
        }
    }

    unsigned int GetVtexRowPitch(
        std::uint8_t format,
        unsigned int width)
    {
        width = (std::max)(1u, width);

        if (format == VTEX_RGBA8888 || format == VTEX_BGRA8888)
            return width * 4u;

        unsigned int blockBytes = 0;

        if (format == VTEX_DXT1 || format == VTEX_ATI1N)
            blockBytes = 8;
        else if (
            format == VTEX_DXT5 ||
            format == VTEX_BC6H ||
            format == VTEX_BC7 ||
            format == VTEX_ATI2N)
        {
            blockBytes = 16;
        }

        if (blockBytes == 0)
            return 0;

        return (std::max)(1u, (width + 3u) / 4u) * blockBytes;
    }

    bool CreateNativeTexture(
        const void* data,
        unsigned int width,
        unsigned int height,
        DXGI_FORMAT format,
        unsigned int rowPitch,
        SkinImages::Image& output)
    {
        output = {};

        if (!g_device || !data || format == DXGI_FORMAT_UNKNOWN || rowPitch == 0)
            return false;

        if (width == 0 || height == 0 ||
            width > MAX_TEXTURE_DIMENSION ||
            height > MAX_TEXTURE_DIMENSION)
        {
            return false;
        }

        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = width;
        textureDesc.Height = height;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = format;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_IMMUTABLE;
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initialData{};
        initialData.pSysMem = data;
        initialData.SysMemPitch = rowPitch;

        ID3D11Texture2D* texture = nullptr;

        HRESULT result =
            g_device->CreateTexture2D(&textureDesc, &initialData, &texture);

        if (FAILED(result) || !texture)
            return false;

        ID3D11ShaderResourceView* view = nullptr;

        result =
            g_device->CreateShaderResourceView(texture, nullptr, &view);

        texture->Release();

        if (FAILED(result) || !view)
            return false;

        output.texture = view;
        output.width = width;
        output.height = height;
        return true;
    }

    bool DecodeEncodedImage(
        const std::uint8_t* data,
        std::size_t size,
        std::vector<std::uint8_t>& rgba,
        unsigned int& width,
        unsigned int& height)
    {
        rgba.clear();
        width = 0;
        height = 0;

        if (!g_wicFactory || !data || size == 0 || size > static_cast<std::size_t>(UINT_MAX))
            return false;

        IWICStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;

        HRESULT result = g_wicFactory->CreateStream(&stream);

        if (SUCCEEDED(result))
        {
            result = stream->InitializeFromMemory(
                const_cast<BYTE*>(reinterpret_cast<const BYTE*>(data)),
                static_cast<DWORD>(size));
        }

        if (SUCCEEDED(result))
        {
            result = g_wicFactory->CreateDecoderFromStream(
                stream,
                nullptr,
                WICDecodeMetadataCacheOnDemand,
                &decoder);
        }

        if (SUCCEEDED(result))
            result = decoder->GetFrame(0, &frame);

        UINT imageWidth = 0;
        UINT imageHeight = 0;

        if (SUCCEEDED(result))
            result = frame->GetSize(&imageWidth, &imageHeight);

        if (SUCCEEDED(result) &&
            (imageWidth == 0 || imageHeight == 0 ||
             imageWidth > MAX_TEXTURE_DIMENSION ||
             imageHeight > MAX_TEXTURE_DIMENSION))
        {
            result = E_FAIL;
        }

        if (SUCCEEDED(result))
            result = g_wicFactory->CreateFormatConverter(&converter);

        if (SUCCEEDED(result))
        {
            result = converter->Initialize(
                frame,
                GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom);
        }

        if (SUCCEEDED(result))
        {
            const std::size_t byteCount =
                static_cast<std::size_t>(imageWidth) *
                static_cast<std::size_t>(imageHeight) * 4u;

            if (byteCount > static_cast<std::size_t>(UINT_MAX))
            {
                result = E_FAIL;
            }
            else
            {
                rgba.resize(byteCount);

                result = converter->CopyPixels(
                    nullptr,
                    imageWidth * 4u,
                    static_cast<UINT>(byteCount),
                    rgba.data());
            }
        }

        if (converter)
            converter->Release();

        if (frame)
            frame->Release();

        if (decoder)
            decoder->Release();

        if (stream)
            stream->Release();

        if (FAILED(result))
        {
            rgba.clear();
            return false;
        }

        width = imageWidth;
        height = imageHeight;
        return true;
    }

    bool ParseVtex(const std::vector<std::uint8_t>& data, VtexInfo& output)
    {
        output = {};

        if (data.size() < 16 || ReadU16(data, 0x04) != 12)
            return false;

        const std::uint32_t blockOffset = ReadU32(data, 0x08);
        const std::uint32_t blockCount = ReadU32(data, 0x0C);

        if (blockCount == 0 || blockCount > 64)
            return false;

        const std::size_t blockTable = static_cast<std::size_t>(blockOffset) + 8;

        std::size_t dataBlockOffset = 0;
        std::uint32_t dataBlockSize = 0;

        for (std::uint32_t i = 0; i < blockCount; ++i)
        {
            const std::size_t entry = blockTable + static_cast<std::size_t>(i) * 12;

            if (entry > data.size() || 12 > data.size() - entry)
                return false;

            const std::uint32_t type = ReadU32(data, entry);
            const std::uint32_t relativeOffset = ReadU32(data, entry + 4);
            const std::uint32_t size = ReadU32(data, entry + 8);

            if (type != RESOURCE_BLOCK_DATA)
                continue;

            dataBlockOffset = entry + 4 + static_cast<std::size_t>(relativeOffset);
            dataBlockSize = size;
            break;
        }

        if (!dataBlockOffset || !dataBlockSize || dataBlockOffset > data.size() || dataBlockSize > data.size() - dataBlockOffset)
            return false;

        if (40 > data.size() - dataBlockOffset || ReadU16(data, dataBlockOffset) != 1)
            return false;

        output.width = ReadU16(data, dataBlockOffset + 20);
        output.height = ReadU16(data, dataBlockOffset + 22);
        output.depth = ReadU16(data, dataBlockOffset + 24);
        output.format = data[dataBlockOffset + 26];
        output.mipLevels = data[dataBlockOffset + 27];

        if (!output.width || !output.height || !output.depth || !output.mipLevels)
            return false;

        output.dataOffset = dataBlockOffset + static_cast<std::size_t>(dataBlockSize);

        const std::uint32_t extraDataOffset = ReadU32(data, dataBlockOffset + 32);
        const std::uint32_t extraDataCount = ReadU32(data, dataBlockOffset + 36);

        if (extraDataCount > 64)
            return false;

        const std::size_t extraBase = dataBlockOffset + 32;

        if (extraBase > data.size() || extraDataOffset > data.size() - extraBase)
            return false;

        const std::size_t extraTable = extraBase + static_cast<std::size_t>(extraDataOffset);

        bool foundCompressedMipInfo = false;

        for (std::uint32_t i = 0; i < extraDataCount; ++i)
        {
            const std::size_t entry = extraTable + static_cast<std::size_t>(i) * 12;

            if (entry > data.size() || 12 > data.size() - entry)
                return false;

            if (ReadU32(data, entry) != VTEX_EXTRA_COMPRESSED_MIP_SIZE)
                continue;

            const std::uint32_t relativeOffset = ReadU32(data, entry + 4);

            if (entry + 4 > data.size() || relativeOffset > data.size() - (entry + 4))
                return false;

            const std::size_t payload = entry + 4 + static_cast<std::size_t>(relativeOffset);

            if (payload > data.size() || 12 > data.size() - payload)
                return false;

            const std::uint32_t compressed = ReadU32(data, payload);
            const std::uint32_t mipsOffset = ReadU32(data, payload + 4);
            const std::uint32_t mipCount = ReadU32(data, payload + 8);

            if ((compressed != 0 && compressed != 1) || mipCount != output.mipLevels || mipCount == 0)
                return false;

            const std::size_t mipBase = payload + 4;

            if (mipBase > data.size() || mipsOffset > data.size() - mipBase)
                return false;

            const std::size_t mipArray = mipBase + static_cast<std::size_t>(mipsOffset);
            const std::size_t mipArraySize = static_cast<std::size_t>(mipCount) * sizeof(std::uint32_t);

            if (mipArray > data.size() || mipArraySize > data.size() - mipArray)
                return false;

            output.compressedMips = compressed == 1;
            output.hasMipSizeTable = true;
            output.mipSizes.resize(mipCount);

            for (std::uint32_t mip = 0; mip < mipCount; ++mip)
                output.mipSizes[mip] = ReadU32(data, mipArray + static_cast<std::size_t>(mip) * sizeof(std::uint32_t));

            foundCompressedMipInfo = true;
            break;
        }

        if (!foundCompressedMipInfo)
        {
            output.compressedMips = false;
            output.hasMipSizeTable = false;
            output.mipSizes.resize(output.mipLevels);

            if (!IsEncodedVtexFormat(output.format))
            {
                for (std::uint32_t mip = 0; mip < output.mipLevels; ++mip)
                {
                    const unsigned int width =
                        (std::max)(1u, static_cast<unsigned int>(output.width) >> mip);
                    const unsigned int height =
                        (std::max)(1u, static_cast<unsigned int>(output.height) >> mip);
                    const unsigned int depth =
                        (std::max)(1u, static_cast<unsigned int>(output.depth) >> mip);

                    const std::size_t mipSize =
                        GetVtexMipDataSize(output.format, width, height, depth);

                    if (mipSize == 0 || mipSize > static_cast<std::size_t>(UINT_MAX))
                        return false;

                    output.mipSizes[mip] =
                        static_cast<std::uint32_t>(mipSize);
                }
            }
        }

        if (output.dataOffset > data.size())
            return false;

        if (IsEncodedVtexFormat(output.format))
            return output.dataOffset < data.size();

        const std::size_t availableData = data.size() - output.dataOffset;
        std::size_t totalStoredSize = 0;

        for (std::uint32_t mip = 0; mip < output.mipLevels; ++mip)
        {
            const unsigned int width =
                (std::max)(1u, static_cast<unsigned int>(output.width) >> mip);
            const unsigned int height =
                (std::max)(1u, static_cast<unsigned int>(output.height) >> mip);
            const unsigned int depth =
                (std::max)(1u, static_cast<unsigned int>(output.depth) >> mip);

            const std::size_t rawSize =
                GetVtexMipDataSize(output.format, width, height, depth);

            if (rawSize == 0)
                return false;

            std::size_t storedSize = rawSize;

            if (output.hasMipSizeTable && mip < output.mipSizes.size())
                storedSize = (std::min)(storedSize, static_cast<std::size_t>(output.mipSizes[mip]));

            if (totalStoredSize > availableData ||
                storedSize > availableData - totalStoredSize)
            {
                return false;
            }

            totalStoredSize += storedSize;
        }

        return true;
    }

    const SkinImages::Image* LoadSvg(const std::string& path)
    {
        if (path.empty() || !g_device || !g_rasteriseSvg)
            return nullptr;

        if (const auto it = g_images.find(path); it != g_images.end())
            return &it->second;

        if (g_failedImages.contains(path))
            return nullptr;

        if (!LoadAllowed())
            return nullptr;   // this frame's loading is used up: next frame

        const LoadTimer timer;
        std::vector<std::uint8_t> fileData;

        if (!ReadGameFile(path, fileData))
        {
            g_failedImages.emplace(path);
            return nullptr;
        }

        std::string svgData;

        if (!ExtractSvgData(fileData, svgData))
        {
            g_failedImages.emplace(path);
            return nullptr;
        }

        if (svgData.size() > static_cast<std::size_t>(UINT_MAX))
        {
            g_failedImages.emplace(path);
            return nullptr;
        }

        CUtlBuffer outputBuffer(0, 0, 0);
        unsigned int width = 0;
        unsigned int height = 0;

        const bool rasterised = g_rasteriseSvg(
            svgData.data(),
            static_cast<unsigned int>(svgData.size()),
            &outputBuffer,
            &width,
            &height,
            SVG_RASTER_SCALE,
            nullptr
        );

        void* rgba = outputBuffer.DetachMemory();

        if (!rasterised || width == 0 || height == 0 || !rgba)
        {
            if (rgba && I::MemFree)
                I::MemFree(rgba);

            g_failedImages.emplace(path);
            return nullptr;
        }

        SkinImages::Image image{};
        const bool created = CreateTexture(rgba, width, height, image);

        I::MemFree(rgba);

        if (!created)
        {
            g_failedImages.emplace(path);
            return nullptr;
        }

        const auto [it, inserted] = g_images.emplace(path, image);

        if (!inserted && image.texture)
            image.texture->Release();

        return &it->second;
    }

    const SkinImages::Image* LoadVtexCached(
        const std::string& path,
        std::unordered_map<std::string, SkinImages::Image>& images,
        std::unordered_set<std::string>& failedImages)
    {
        if (path.empty() || !g_device)
            return nullptr;

        if (const auto it = images.find(path); it != images.end())
            return &it->second;

        if (failedImages.contains(path))
            return nullptr;

        if (!LoadAllowed())
            return nullptr;   // this frame's loading is used up: next frame

        const LoadTimer timer;
        std::vector<std::uint8_t> fileData;

        if (!ReadGameFile(path, fileData))
        {
            failedImages.emplace(path);
            return nullptr;
        }

        VtexInfo info{};

        if (!ParseVtex(fileData, info) || info.depth != 1)
        {
            failedImages.emplace(path);
            return nullptr;
        }

        SkinImages::Image image{};

        if (IsEncodedVtexFormat(info.format))
        {
            std::vector<std::uint8_t> rgba;
            unsigned int width = 0;
            unsigned int height = 0;

            if (!DecodeEncodedImage(
                    fileData.data() + info.dataOffset,
                    fileData.size() - info.dataOffset,
                    rgba,
                    width,
                    height) ||
                !CreateTexture(rgba.data(), width, height, image))
            {
                failedImages.emplace(path);
                return nullptr;
            }
        }
        else
        {
            const DXGI_FORMAT dxgiFormat = GetVtexDxgiFormat(info.format);

            if (dxgiFormat == DXGI_FORMAT_UNKNOWN)
            {
                failedImages.emplace(path);
                return nullptr;
            }

            std::uint32_t selectedMip = 0;

            while (selectedMip + 1 < info.mipLevels)
            {
                const std::uint32_t nextHeight =
                    (std::max)(
                        1u,
                        static_cast<unsigned int>(info.height) >>
                            (selectedMip + 1));

                if (nextHeight < 128)
                    break;

                ++selectedMip;
            }

            const unsigned int width =
                (std::max)(
                    1u,
                    static_cast<unsigned int>(info.width) >> selectedMip);

            const unsigned int height =
                (std::max)(
                    1u,
                    static_cast<unsigned int>(info.height) >> selectedMip);

            const unsigned int depth =
                (std::max)(
                    1u,
                    static_cast<unsigned int>(info.depth) >> selectedMip);

            const std::size_t decodedSize =
                GetVtexMipDataSize(info.format, width, height, depth);

            if (decodedSize == 0)
            {
                failedImages.emplace(path);
                return nullptr;
            }

            std::size_t sourceOffset = info.dataOffset;

            for (int mip = static_cast<int>(info.mipLevels) - 1;
                 mip > static_cast<int>(selectedMip);
                 --mip)
            {
                const unsigned int mipWidth =
                    (std::max)(
                        1u,
                        static_cast<unsigned int>(info.width) >>
                            static_cast<unsigned int>(mip));

                const unsigned int mipHeight =
                    (std::max)(
                        1u,
                        static_cast<unsigned int>(info.height) >>
                            static_cast<unsigned int>(mip));

                const unsigned int mipDepth =
                    (std::max)(
                        1u,
                        static_cast<unsigned int>(info.depth) >>
                            static_cast<unsigned int>(mip));

                const std::size_t rawSize =
                    GetVtexMipDataSize(
                        info.format,
                        mipWidth,
                        mipHeight,
                        mipDepth);

                if (rawSize == 0)
                {
                    failedImages.emplace(path);
                    return nullptr;
                }

                std::size_t storedSize = rawSize;

                if (info.hasMipSizeTable &&
                    static_cast<std::size_t>(mip) < info.mipSizes.size())
                {
                    storedSize =
                        (std::min)(
                            storedSize,
                            static_cast<std::size_t>(
                                info.mipSizes[
                                    static_cast<std::size_t>(mip)]));
                }

                if (sourceOffset > fileData.size() ||
                    storedSize > fileData.size() - sourceOffset)
                {
                    failedImages.emplace(path);
                    return nullptr;
                }

                sourceOffset += storedSize;
            }

            std::size_t sourceSize = decodedSize;

            if (info.hasMipSizeTable &&
                selectedMip < info.mipSizes.size())
            {
                sourceSize =
                    (std::min)(
                        sourceSize,
                        static_cast<std::size_t>(
                            info.mipSizes[selectedMip]));
            }

            if (sourceOffset > fileData.size() ||
                sourceSize > fileData.size() - sourceOffset)
            {
                failedImages.emplace(path);
                return nullptr;
            }

            std::vector<std::uint8_t> textureData(decodedSize);

            if (info.compressedMips && sourceSize < decodedSize)
            {
                if (DecompressLz4(
                        fileData.data() + sourceOffset,
                        sourceSize,
                        textureData.data(),
                        textureData.size()) !=
                    static_cast<int>(decodedSize))
                {
                    failedImages.emplace(path);
                    return nullptr;
                }
            }
            else
            {
                if (sourceSize < decodedSize)
                {
                    failedImages.emplace(path);
                    return nullptr;
                }

                std::memcpy(
                    textureData.data(),
                    fileData.data() + sourceOffset,
                    decodedSize);
            }

            const unsigned int rowPitch =
                GetVtexRowPitch(info.format, width);

            if (!CreateNativeTexture(
                    textureData.data(),
                    width,
                    height,
                    dxgiFormat,
                    rowPitch,
                    image))
            {
                failedImages.emplace(path);
                return nullptr;
            }
        }

        const auto [it, inserted] =
            images.emplace(path, image);

        if (!inserted && image.texture)
            image.texture->Release();

        return &it->second;
    }

    const SkinImages::Image* LoadVtex(const std::string& path)
    {
        return LoadVtexCached(path, g_images, g_failedImages);
    }

}

void SkinImages::BeginFrame()
{
    g_loadSpentMs = 0.0;
    g_loadsThisFrame = 0;
}

bool SkinImages::Initialize(ID3D11Device* device)
{
    if (!device || !I::FileSystem || !I::ConstructUtlBuffer || !I::DetachUtlBufferMemory || !I::MemFree)
        return false;

    if (g_device && g_rasteriseSvg)
        return true;

    Shutdown();

    g_rasteriseSvg = reinterpret_cast<RasteriseSvgFn>(M::scan("panorama.dll", Patterns::RasteriseSvg));

    if (!g_rasteriseSvg)
        return false;

    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (SUCCEEDED(comResult))
        g_comInitialized = true;

    CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&g_wicFactory));

    g_device = device;
    g_device->AddRef();

    return true;
}

void SkinImages::Shutdown()
{
    for (auto& [path, image] : g_images)
    {
        if (image.texture)
            image.texture->Release();
    }

    for (auto& [path, image] : g_stickerImages)
    {
        if (image.texture)
            image.texture->Release();
    }

    g_images.clear();
    g_failedImages.clear();
    g_stickerImages.clear();
    g_failedStickerImages.clear();
    g_stickerResolvedPaths.clear();

    if (g_wicFactory)
    {
        g_wicFactory->Release();
        g_wicFactory = nullptr;
    }

    if (g_comInitialized)
    {
        CoUninitialize();
        g_comInitialized = false;
    }

    if (g_device)
    {
        g_device->Release();
        g_device = nullptr;
    }

    g_rasteriseSvg = nullptr;
}

void SkinImages::PrecacheAll()
{
    if (!g_device || !g_rasteriseSvg)
        return;

    for (const auto& item : Skins::g_ItemSchema.GetItems())
    {
        if (!item.isWeapon && !item.isKnife && !item.isGlove && !item.isAgent)
            continue;

        if (item.isAgent && !item.hasUsableAgentModel)
            continue;

        GetModelPreview(item);

        if (item.isAgent)
            continue;

        const std::vector<PaintKitInfo>* paintKits = Skins::g_ItemSchema.GetPaintKitsForItem(item.definitionIndex);

        if (!paintKits)
            continue;

        for (const PaintKitInfo& paintKit : *paintKits)
        {
            if (paintKit.id > 0)
                GetPaintKitImage(item, paintKit);
        }
    }
}

namespace
{
    // Music kits and charms both carry an items_game "image_inventory" path → panorama/images/<path>_png.vtex_c
    const SkinImages::Image* LoadInventoryImage(const std::string& image)
    {
        if (!g_device || image.empty())
            return nullptr;

        std::string path = "panorama/images/";
        path.reserve(path.size() + image.size() + 12);
        path += image;
        path += "_png.vtex_c";
        return LoadVtex(path);
    }
}

const SkinImages::Image* SkinImages::GetMusicKitImage(const MusicKitInfo& kit)
{
    return LoadInventoryImage(kit.image);
}

const SkinImages::Image* SkinImages::GetKeychainImage(const KeychainInfo& keychain)
{
    return LoadInventoryImage(keychain.image);
}

const SkinImages::Image* SkinImages::GetInventoryImage(const std::string& image)
{
    return LoadInventoryImage(image);
}

const SkinImages::Image* SkinImages::GetEncodedImage(const std::string& key, const std::uint8_t* data, std::size_t size)
{
    if (!g_device || key.empty() || !data || size == 0)
        return nullptr;

    const std::string cacheKey = "embedded:" + key;

    if (const auto it = g_images.find(cacheKey); it != g_images.end())
        return &it->second;

    if (g_failedImages.contains(cacheKey))
        return nullptr;

    if (!LoadAllowed())
        return nullptr;   // this frame's loading is used up: next frame

    const LoadTimer timer;
    std::vector<std::uint8_t> rgba;
    unsigned int width = 0;
    unsigned int height = 0;
    Image image{};

    if (!DecodeEncodedImage(data, size, rgba, width, height) || !CreateTexture(rgba.data(), width, height, image))
    {
        g_failedImages.emplace(cacheKey);
        return nullptr;
    }

    return &g_images.emplace(cacheKey, image).first->second;
}

const SkinImages::Image* SkinImages::GetUiIcon(const std::string& name)
{
    if (name.empty() || !g_device || !g_rasteriseSvg)
        return nullptr;

    return LoadSvg("panorama/images/icons/ui/" + name + ".vsvg_c");
}

const SkinImages::Image* SkinImages::GetItemIcon(const ItemDefinitionInfo& item)
{
    if (!g_device || !g_rasteriseSvg)
        return nullptr;

    const std::string path = BuildEquipmentIconPath(item);
    return path.empty() ? nullptr : LoadSvg(path);
}

const SkinImages::Image* SkinImages::GetPaintKitImage(const ItemDefinitionInfo& item, const PaintKitInfo& paintKit)
{
    if (paintKit.id == 0)
        return GetItemIcon(item);

    const std::string path = BuildPaintKitImagePath(item, paintKit);
    return path.empty() ? nullptr : LoadVtex(path);
}

const SkinImages::Image* SkinImages::GetModelPreview(const ItemDefinitionInfo& item)
{
    if (item.isAgent)
    {
        const std::string path = BuildAgentImagePath(item);
        return path.empty() ? nullptr : LoadVtex(path);
    }

    if (item.isWeapon)
    {
        const std::string path = BuildBaseWeaponImagePath(item);

        if (!path.empty())
        {
            if (const Image* image = LoadVtex(path))
                return image;
        }

        return GetItemIcon(item);
    }

    if (!item.isKnife && !item.isGlove)
        return GetItemIcon(item);

    const int paintKitId = item.isKnife ? GetKnifePreviewPaintKitId(item.definitionIndex) : GetGlovePreviewPaintKitId(item.definitionIndex);

    if (paintKitId <= 0)
        return nullptr;

    const PaintKitInfo* paintKit = Skins::g_ItemSchema.GetPaintKit(paintKitId);
    return paintKit ? GetPaintKitImage(item, *paintKit) : nullptr;
}

void SkinImages::UpdateStickerCache(const std::vector<const StickerKitInfo*>& stickers)
{
    std::vector<std::string> orderedPaths;
    orderedPaths.reserve(stickers.size());

    std::unordered_set<std::string> keep;
    keep.reserve(stickers.size());

    for (const StickerKitInfo* sticker : stickers)
    {
        if (!sticker)
            continue;

        std::string path = ResolveStickerImagePath(*sticker);

        if (!path.empty() && keep.emplace(path).second)
            orderedPaths.emplace_back(std::move(path));
    }

    for (auto it = g_stickerImages.begin(); it != g_stickerImages.end();)
    {
        if (keep.contains(it->first))
        {
            ++it;
            continue;
        }

        if (it->second.texture)
            it->second.texture->Release();

        it = g_stickerImages.erase(it);
    }

    std::size_t loadAttempts = 0;

    for (const std::string& path : orderedPaths)
    {
        if (g_stickerImages.contains(path) || g_failedStickerImages.contains(path))
            continue;

        LoadVtexCached(path, g_stickerImages, g_failedStickerImages);

        if (++loadAttempts >= STICKER_LOAD_BUDGET_PER_FRAME)
            break;
    }
}

void SkinImages::ClearStickerCache()
{
    for (auto& [path, image] : g_stickerImages)
    {
        if (image.texture)
            image.texture->Release();
    }

    g_stickerImages.clear();
}

const SkinImages::Image* SkinImages::GetStickerImage(const StickerKitInfo& sticker)
{
    const std::string& path = ResolveStickerImagePath(sticker);

    if (path.empty())
        return nullptr;

    const auto it = g_stickerImages.find(path);
    return it != g_stickerImages.end() ? &it->second : nullptr;
}
