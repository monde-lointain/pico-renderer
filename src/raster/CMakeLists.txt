add_library(Renderer_raster)
add_library(Renderer::Raster ALIAS Renderer_raster)
target_sources(Renderer_raster PRIVATE raster.cc)
target_include_directories(Renderer_raster PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(Renderer_raster
    PUBLIC renderer::sanitizers
    # R.1: raster_one composes the texture sampler + color combiner + the
    # rgb565() packer (gfx/framebuffer.h). None is part of raster.h's interface
    # -> PRIVATE; CMake still propagates the static-lib link requirement to the
    # final executable. rgb565 is a header-only static inline (no link symbol),
    # but link renderer::gfx explicitly so the dependency is declared (no
    # cycle: gfx does not depend on raster).
    # L6: the XLU sweep composes Renderer::Blend's front-to-back premultiplied-
    # UNDER helpers (blend_premul_accumulate/_resolve) + fog_lerp; also PRIVATE
    # (not in raster.h's interface). blend_pixel_alpha is retained in the Blend
    # module (still used by tests + a valid primitive) but no longer called from
    # the XLU sweep.
    # R.3-AA: the coverage WRITE + the end-of-tile resolve compose Renderer::Aa
    # (aa_enabled / aa_resolve_tile / AA_COV_FULL); PRIVATE — none is in
    # raster.h's interface (the nullable cov param is just a uint8_t*).
    # T5: PROF_RASTER_TILE (coarse) + per-sweep FINE blocks (SWEEP_OPAQUE/
    # SWEEP_XLU/AA_RESOLVE) instrument the back-end hot path. prof.h is .cc-only
    # -> PRIVATE; PROFILER=0 (golden/CI default) -> macros expand to ((void)0).
    PRIVATE Renderer::Tex Renderer::Shade Renderer::Blend renderer::gfx
            Renderer::Aa Renderer::Prof)
orthodoxy_enforce(Renderer_raster)
pico_tidy_enable(Renderer_raster)
