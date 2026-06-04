# Granular intra-frame profiler (T5). Opt-in, target-scoped — mirrors
# Sanitizers.cmake. OFF by default so golden / CI / default builds are
# bit-identical: when off the TimeBlock* macros expand to ((void)0) (zero
# codegen) and the public API are empty inline stubs, so the profiler never
# touches the framebuffer / zbuf / coverage path.
#
# Enable with -DRENDERER_PROFILER=ON (the pico-prof preset) to bake PROFILER=1
# into the instrumented TUs via the always-present renderer::prof_config
# interface target — a single source of truth carried PUBLIC by Renderer::Prof,
# so every instrumented consumer (raster / rdr / sched / demo) inherits it
# transitively.
option(RENDERER_PROFILER
  "Enable the granular intra-frame profiler (PROFILER=1 in instrumented TUs)"
  OFF)

# Always define the interface target so the PUBLIC link in Renderer::Prof is
# valid even when the profiler is off (then it carries no macro = no-op).
add_library(renderer_prof_config INTERFACE)
add_library(renderer::prof_config ALIAS renderer_prof_config)

if(RENDERER_PROFILER)
  target_compile_definitions(renderer_prof_config INTERFACE PROFILER=1)
  message(STATUS "Profiler enabled: PROFILER=1 (instrumented TUs carry RAII scope timers)")
endif()
