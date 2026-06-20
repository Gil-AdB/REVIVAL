#ifndef FDS_FEATURE_FLAGS_H_INCLUDED
#define FDS_FEATURE_FLAGS_H_INCLUDED

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

// Compile-time defaults consumed by FeatureFlags.def. Defined here (not in
// FDS_DECS.H) so the .def is self-contained — anyone including FeatureFlags.h
// gets the macros resolved correctly. The WASM build flips these via
// -DFDS_*_DEFAULT_ON=1 in FDS/CMakeLists.txt without touching the table.
#ifndef FDS_DEFERRED_DEFAULT_ON
#define FDS_DEFERRED_DEFAULT_ON 0
#endif
#ifndef FDS_SHADOWS_DEFAULT_ON
#define FDS_SHADOWS_DEFAULT_ON 0
#endif
#ifndef FDS_SHADOW_POLYID_DEFAULT_ON
#define FDS_SHADOW_POLYID_DEFAULT_ON 1
#endif

namespace fds {

// Table-driven registry for engine feature flags.
//
// Lookup order, highest precedence first:
//   1. Command-line  (--name / --no-name / --name=value)
//   2. Environment   (legacy FDS_* names preserved)
//   3. Compile-time default from FeatureFlags.def
//
// Accessors are cheap: a single array load. Flag state is frozen after
// parseArgs() — flags do not change at runtime.
class FeatureFlags {
public:
    enum class BoolId : int {
        #define FDS_FLAG_BOOL(name, env, def, cat, help) name,
        #define FDS_FLAG_FLOAT(name, env, def, cat, help)
        #define FDS_FLAG_INT(name, env, def, cat, help)
        #include "FeatureFlags.def"
        #undef FDS_FLAG_BOOL
        #undef FDS_FLAG_FLOAT
        #undef FDS_FLAG_INT
        Count
    };

    enum class FloatId : int {
        #define FDS_FLAG_BOOL(name, env, def, cat, help)
        #define FDS_FLAG_FLOAT(name, env, def, cat, help) name,
        #define FDS_FLAG_INT(name, env, def, cat, help)
        #include "FeatureFlags.def"
        #undef FDS_FLAG_BOOL
        #undef FDS_FLAG_FLOAT
        #undef FDS_FLAG_INT
        Count
    };

    enum class IntId : int {
        #define FDS_FLAG_BOOL(name, env, def, cat, help)
        #define FDS_FLAG_FLOAT(name, env, def, cat, help)
        #define FDS_FLAG_INT(name, env, def, cat, help) name,
        #include "FeatureFlags.def"
        #undef FDS_FLAG_BOOL
        #undef FDS_FLAG_FLOAT
        #undef FDS_FLAG_INT
        Count
    };

    // Parses --flag / --no-flag / --flag=value forms. Recognises --help / -h.
    // Returns true on success, false when --help was requested or an unknown
    // flag was seen (in which case help has already been printed to stderr).
    static bool parseArgs(int argc, const char *const *argv);

    // Prints the full flag table to `out`, grouped by category.
    static void printHelp(std::FILE *out = stderr);

    // Prints flags that differ from compile-time defaults (set via env or CLI).
    static void printActive(std::FILE *out = stderr);

    // Emits a bash/zsh completion script listing every --flag and --no-<bool>
    // form. `shell` is "bash" or "zsh" (others fall back to bash syntax).
    // Source the output once per session: `source <(./DEMO --print-completion)`.
    static void printCompletion(std::FILE *out, const char *shell);

    // ── Param-script registry lookup (see Base/ParamScript.cpp) ────────
    // Resolves a flag NAME (dash or underscore form) to its type + index
    // into the g_* arrays below, so scripts can read/write any flag by
    // name without compile-time ids. type == None when no such flag.
    enum class ParamType : int { None = 0, Bool, Float, Int };
    struct ParamRef { ParamType type = ParamType::None; int index = -1; };
    static ParamRef findParam(const char *name);

    // Hot-path accessors: inline reads from process-lifetime globals.
    // Flag state is set at startup (env scan + parseArgs) and frozen
    // after that — no per-call function-static guard needed. See
    // FeatureFlags.cpp for the globals' definition and one-time init.
    //
    // The arrays are accessed by index; static_cast<int>(id) is the
    // enum value. kNum* dimensions are not exposed in the header to
    // avoid leaking the per-category sizes; the arrays are dynamically
    // sized in the cpp but accessed by index here. The id values are
    // bounded by the enum's Count, which the cpp asserts at init.
    static bool * const g_boolVals;
    static float * const g_floatVals;
    static int   * const g_intVals;
    static bool * const g_boolSet;
    static bool * const g_floatSet;
    static bool * const g_intSet;

    static inline bool  get(BoolId  id) { return g_boolVals [static_cast<int>(id)]; }
    static inline float get(FloatId id) { return g_floatVals[static_cast<int>(id)]; }
    static inline int   get(IntId   id) { return g_intVals  [static_cast<int>(id)]; }

    // True if the flag was explicitly set on the CLI or in the environment
    // (i.e. its value is not the compile-time default).
    static inline bool isSet(BoolId  id) { return g_boolSet [static_cast<int>(id)]; }
    static inline bool isSet(FloatId id) { return g_floatSet[static_cast<int>(id)]; }
    static inline bool isSet(IntId   id) { return g_intSet  [static_cast<int>(id)]; }

    // Scene-driver default override: set a flag's value UNLESS the
    // user explicitly set it (CLI/env). Lets a scene tune a global
    // (e.g. greets's disco-beam cone_strength) without trampling
    // user overrides. Does NOT mark the flag as set.
    static inline void setDefault(BoolId  id, bool v)  { if (!isSet(id)) g_boolVals [static_cast<int>(id)] = v; }
    static inline void setDefault(FloatId id, float v) { if (!isSet(id)) g_floatVals[static_cast<int>(id)] = v; }
    static inline void setDefault(IntId   id, int v)   { if (!isSet(id)) g_intVals  [static_cast<int>(id)] = v; }

// ── Tune-server support (see Base/TuneServer.cpp) ──────────────────
    // dumpParamsJson appends a JSON array of every flag: name, type,
    // current value, default, category, help, and whether it's explicitly
    // set (CLI/env/web). setParamFromText writes a value by name and marks
    // it SET (same precedence as the CLI — param scripts yield to it);
    // unsetParam clears the mark and restores the compile-time default,
    // handing control back to scripts/defaults.
    static void dumpParamsJson(std::string &out);
    // Every explicitly-set param as (name, value-text). Used by the
    // console's bake-to-script.
    static void dumpSetParams(std::vector<std::pair<std::string, std::string>> &out);
    // Clear the SET mark but KEEP the current value (bake handoff: the
    // just-written script line takes over on its next tick with the same
    // value — no flash through the compile default).
    static bool clearSetMark(const char *name);
    static bool setParamFromText(const char *name, const char *value);
    static bool unsetParam(const char *name);

    // Generated per-flag convenience accessors. Usage: FeatureFlags::deferred().
    #define FDS_FLAG_BOOL(name, env, def, cat, help) \
        static bool name() { return get(BoolId::name); }
    #define FDS_FLAG_FLOAT(name, env, def, cat, help) \
        static float name() { return get(FloatId::name); }
    #define FDS_FLAG_INT(name, env, def, cat, help) \
        static int name() { return get(IntId::name); }
    #include "FeatureFlags.def"
    #undef FDS_FLAG_BOOL
    #undef FDS_FLAG_FLOAT
    #undef FDS_FLAG_INT
};

} // namespace fds

#endif
