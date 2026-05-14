#ifndef FDS_FEATURE_FLAGS_H_INCLUDED
#define FDS_FEATURE_FLAGS_H_INCLUDED

#include <cstdio>

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

    static bool  get(BoolId  id);
    static float get(FloatId id);
    static int   get(IntId   id);

    // True if the flag was explicitly set on the CLI or in the environment
    // (i.e. its value is not the compile-time default).
    static bool isSet(BoolId  id);
    static bool isSet(FloatId id);
    static bool isSet(IntId   id);

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
