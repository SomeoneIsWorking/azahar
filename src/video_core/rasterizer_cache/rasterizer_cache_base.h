// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include <boost/icl/interval_map.hpp>
#include <tsl/robin_map.h>

#include "video_core/rasterizer_cache/framebuffer_base.h"
#include "video_core/rasterizer_cache/sampler_params.h"
#include "video_core/rasterizer_cache/surface_base.h"
#include "video_core/rasterizer_cache/surface_params.h"
#include "video_core/rasterizer_cache/texture_cube.h"

namespace Memory {
class MemorySystem;
}

namespace Pica {
struct RegsInternal;
struct DisplayTransferConfig;
struct MemoryFillConfig;
} // namespace Pica

namespace Pica::Texture {
struct TextureInfo;
}

namespace Settings {
enum class TextureFilter : u32;
}

namespace VideoCore {

enum class ScaleMatch {
    Exact,   ///< Only accept same res scale
    Upscale, ///< Only allow higher scale than params
    Ignore   ///< Accept every scaled res
};

enum class MatchFlags {
    Exact = 1 << 0,       ///< Surface perfectly matches params
    SubRect = 1 << 1,     ///< Surface encompasses params
    Copy = 1 << 2,        ///< Surface that can be used as a copy source
    TexCopy = 1 << 3,     ///< Surface that will match a display transfer "texture copy" parameters
    Reinterpret = 1 << 4, ///< Surface might have different pixel format.
};

DECLARE_ENUM_FLAG_OPERATORS(MatchFlags);

class CustomTexManager;
class RendererBase;
enum class SurfaceFlagBits : u32;

template <class T>
class RasterizerCache {
    /// Address shift for caching surfaces into a hash table
    static constexpr u64 CITRA_PAGEBITS = 18;

    using Runtime = typename T::Runtime;
    using Sampler = typename T::Sampler;
    using Surface = typename T::Surface;
    using Framebuffer = typename T::Framebuffer;
    using DebugScope = typename T::DebugScope;

    using SurfaceMap = boost::icl::interval_map<PAddr, SurfaceId, boost::icl::partial_absorber,
                                                std::less, boost::icl::inplace_plus,
                                                boost::icl::inter_section, SurfaceInterval>;

    using SurfaceRect_Tuple = std::pair<SurfaceId, Common::Rectangle<u32>>;
    using PageMap = boost::icl::interval_map<u32, int>;

public:
    explicit RasterizerCache(Memory::MemorySystem& memory, CustomTexManager& custom_tex_manager,
                             Runtime& runtime, Pica::RegsInternal& regs, RendererBase& renderer);
    ~RasterizerCache();

    /// Notify the cache that a new frame has been queued
    void TickFrame();

    /// Perform hardware accelerated texture copy according to the provided configuration
    bool AccelerateTextureCopy(const Pica::DisplayTransferConfig& config);

    /// Perform hardware accelerated display transfer according to the provided configuration
    bool AccelerateDisplayTransfer(const Pica::DisplayTransferConfig& config);

    /// Perform a display transfer into `target`, a surface the cache does not track, made to match
    /// the transfer's output; the guest's output memory is left alone
    bool DisplayTransferInto(const Pica::DisplayTransferConfig& config,
                             std::optional<Surface>& target);

    /// Perform hardware accelerated memory fill according to the provided configuration
    bool AccelerateFill(const Pica::MemoryFillConfig& config);

    /// Returns a reference to the surface object assigned to surface_id
    Surface& GetSurface(SurfaceId surface_id);

    /// Returns a reference to the sampler object matching the provided configuration
    Sampler& GetSampler(const Pica::TexturingRegs::TextureConfig& config);
    Sampler& GetSampler(SamplerId sampler_id);

    /// Copy one surface's region to another
    void CopySurface(Surface& src_surface, Surface& dst_surface, SurfaceInterval copy_interval);

    /// Load a texture from 3DS memory to OpenGL and cache it (if not already cached)
    SurfaceId GetSurface(const SurfaceParams& params, ScaleMatch match_res_scale,
                         bool load_if_create, const SurfaceFlagBits& create_initial_flags = {});

    /// Attempt to find a subrect (resolution scaled) of a surface, otherwise loads a texture from
    /// 3DS memory to OpenGL and caches it (if not already cached)
    SurfaceRect_Tuple GetSurfaceSubRect(const SurfaceParams& params, ScaleMatch match_res_scale,
                                        bool load_if_create,
                                        const SurfaceFlagBits& create_initial_flags = {});

    /// Get a surface based on the texture configuration
    Surface& GetTextureSurface(const Pica::TexturingRegs::FullTextureConfig& config);
    SurfaceId GetTextureSurface(const Pica::Texture::TextureInfo& info, u32 max_level = 0);

    /// Get a texture cube based on the texture configuration
    Surface& GetTextureCube(const TextureCubeConfig& config);

    /// Get the color and depth surfaces based on the framebuffer configuration
    FramebufferHelper<T> GetFramebufferSurfaces(bool using_color_fb, bool using_depth_fb);

    /// Get a surface that matches a "texture copy" display transfer config
    SurfaceRect_Tuple GetTexCopySurface(const SurfaceParams& params);

    /// Write any cached resources overlapping the region back to memory (if dirty)
    void FlushRegion(PAddr addr, u32 size, SurfaceId flush_surface = {});

    /// Mark region as being invalidated by region_owner (nullptr if 3DS memory)
    void InvalidateRegion(PAddr addr, u32 size, SurfaceId region_owner = {});

    /// Flush all cached resources tracked by this cache manager
    void FlushAll();

    /// Opens the overlay a replay runs in: until CloseOverlay, the first image write to a surface
    /// that existed at the open goes to a copy that takes its slot. Closing puts the surfaces,
    /// their registration, the dirty regions and the texture cubes back as they were at the open,
    /// and retires every surface made meanwhile.
    void OpenOverlay();
    void CloseOverlay();

    /// The registered surfaces with their contents' hashes, the dirty regions, the cached pages
    /// and the texture cubes, read back for a leak check
    std::map<std::string, std::string> CaptureState();

    /// Clear all cached resources tracked by this cache manager
    void ClearAll(bool flush);

private:
    /// A display transfer's source and the parameters of its output
    struct DisplayTransferPlan {
        SurfaceId src_surface_id;
        Common::Rectangle<u32> src_rect;
        SurfaceParams src_params;
        SurfaceParams dst_params;
    };
    std::optional<DisplayTransferPlan> PlanDisplayTransfer(
        const Pica::DisplayTransferConfig& config);
    bool BlitDisplayTransfer(const Pica::DisplayTransferConfig& config,
                             const DisplayTransferPlan& plan, Surface& dst_surface,
                             Common::Rectangle<u32> dst_rect, u32 dst_level);

    /// Iterate over all page indices in a range
    template <typename Func>
    void ForEachPage(PAddr addr, std::size_t size, Func&& func) {
        static constexpr bool RETURNS_BOOL = std::is_same_v<std::invoke_result<Func, u64>, bool>;
        const u64 page_end = (addr + size - 1) >> CITRA_PAGEBITS;
        for (u64 page = addr >> CITRA_PAGEBITS; page <= page_end; ++page) {
            if constexpr (RETURNS_BOOL) {
                if (func(page)) {
                    break;
                }
            } else {
                func(page);
            }
        }
    }

    /// Iterates over all the surfaces in a region calling func
    template <typename Func>
    void ForEachSurfaceInRegion(PAddr addr, std::size_t size, Func&& func);

    /// Get the best surface match (and its match type) for the given flags
    template <MatchFlags find_flags>
    SurfaceId FindMatch(const SurfaceParams& params, ScaleMatch match_scale_type,
                        std::optional<SurfaceInterval> validate_interval = std::nullopt);

    /// Unregisters sentenced surfaces that have surpassed the destruction threshold.
    void RunGarbageCollector();

    /// Removes any framebuffers that reference the provided surface_id; their objects are
    /// destroyed once the GPU is done with them.
    void RemoveFramebuffers(SurfaceId surface_id);

    /// Before an image write to surface_id: a surface the open overlay keeps is copied into a new
    /// slot first and the copy takes its place, so the write lands on the copy.
    void PrepareWrite(SurfaceId surface_id);

    /// Removes any references of the provided surface id from cached texture cubes.
    void RemoveTextureCubeFace(SurfaceId surface_id);

    /// Computes the hash of the provided texture data.
    u64 ComputeHash(const SurfaceParams& load_info, std::span<u8> upload_data);

    /// Update surface's texture for given region when necessary
    void ValidateSurface(SurfaceId surface, PAddr addr, u32 size);

    /// Copies pixel data in interval from the guest VRAM to the host GPU surface
    void UploadSurface(Surface& surface, SurfaceInterval interval);

    /// Uploads a custom texture identified with hash to the target surface
    bool UploadCustomSurface(SurfaceId surface_id, SurfaceInterval interval);

    /// Copies pixel data in interval from the host GPU surface to the guest VRAM
    void DownloadSurface(Surface& surface, SurfaceInterval interval);

    /// Downloads a fill surface to guest VRAM
    void DownloadFillSurface(Surface& surface, SurfaceInterval interval);

    /// Attempt to find a reinterpretable surface in the cache and use it to copy for validation
    bool ValidateByReinterpretation(Surface& surface, SurfaceParams params,
                                    const SurfaceInterval& interval);

    /// Create a new surface
    SurfaceId CreateSurface(const SurfaceParams& params, const SurfaceFlagBits& initial_flags = {});

    /// Register surface into the cache
    void RegisterSurface(SurfaceId surface);

    /// Remove surface from the cache
    void UnregisterSurface(SurfaceId surface);

    /// Unregisters all surfaces from the cache
    void UnregisterAll();

    /// Unregisters the replaced textures that a texture variant changes
    void UnregisterVariantSurfaces();

    /// Increase/decrease the number of surface in pages touching the specified region
    void UpdatePagesCachedCount(PAddr addr, u32 size, int delta);

private:
    Memory::MemorySystem& memory;
    CustomTexManager& custom_tex_manager;
    Runtime& runtime;
    Pica::RegsInternal& regs;
    RendererBase& renderer;
    std::unordered_map<TextureCubeConfig, TextureCube> texture_cube_cache;
    tsl::robin_pg_map<u64, std::vector<SurfaceId>, Common::IdentityHash<u64>> page_table;
    std::unordered_map<FramebufferParams, FramebufferId> framebuffers;
    std::unordered_map<SamplerParams, SamplerId> samplers;
    std::list<std::pair<SurfaceId, u64>> sentenced;
    std::list<std::pair<FramebufferId, u64>> sentenced_framebuffers;
    /// What an open overlay puts back on close.
    struct Overlay {
        /// The metadata of the surfaces registered or in a texture cube at the open, by slot.
        std::unordered_map<u32, SurfaceBase> kept;
        /// The kept surfaces written meanwhile: the slot that holds the original, by slot.
        std::unordered_map<u32, SurfaceId> originals;
        /// The kept surfaces unregistered meanwhile.
        std::vector<SurfaceId> unregistered;
        SurfaceMap dirty_regions;
        std::unordered_map<TextureCubeConfig, TextureCube> texture_cube_cache;
    };
    std::optional<Overlay> overlay;
    Common::SlotVector<Surface> slot_surfaces;
    Common::SlotVector<Sampler> slot_samplers;
    Common::SlotVector<Framebuffer> slot_framebuffers;
    SurfaceMap dirty_regions;
    PageMap cached_pages;
    u32 resolution_scale_factor;
    FramebufferParams fb_params;
    Settings::TextureFilter filter;
    bool dump_textures;
    bool use_custom_textures;
};

} // namespace VideoCore
