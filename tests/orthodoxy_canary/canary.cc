// Orthodoxy enforcement-active canary.
//
// This translation unit contains DELIBERATE Orthodox C++ violations that the
// Orthodoxy clang plugin (.orthodoxy.yml: Auto:false, Class:false,
// NamedCast:false) MUST reject. It is compiled ONLY when the plugin is active
// and ONLY by the renderer_orthodoxy_canary ctest, which is marked
// WILL_FAIL.
//
// If this file ever compiles successfully while the plugin is supposed to be
// active, enforcement is silently dead — the canary turns the ctest red.
//
// NOTE: the violations below intentionally carry NO `HERESY(...)` suppression
// comment; a suppression would defeat the canary.

int canary() {
  auto x = 1;                  // Auto: forbidden
  return static_cast<int>(x);  // NamedCast: forbidden
}

class Forbidden {  // Class: forbidden
  int value;
};
