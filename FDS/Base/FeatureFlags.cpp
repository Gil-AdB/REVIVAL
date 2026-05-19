#include "FeatureFlags.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fds {
namespace {

struct BoolDef {
    const char *name;
    const char *envVar;
    bool        defaultValue;
    const char *category;
    const char *help;
};
struct FloatDef {
    const char *name;
    const char *envVar;
    float       defaultValue;
    const char *category;
    const char *help;
};
struct IntDef {
    const char *name;
    const char *envVar;
    int         defaultValue;
    const char *category;
    const char *help;
};

constexpr BoolDef kBoolDefs[] = {
    #define FDS_FLAG_BOOL(name, env, def, cat, help) { #name, env, !!(def), cat, help },
    #define FDS_FLAG_FLOAT(name, env, def, cat, help)
    #define FDS_FLAG_INT(name, env, def, cat, help)
    #include "FeatureFlags.def"
    #undef FDS_FLAG_BOOL
    #undef FDS_FLAG_FLOAT
    #undef FDS_FLAG_INT
};
constexpr FloatDef kFloatDefs[] = {
    #define FDS_FLAG_BOOL(name, env, def, cat, help)
    #define FDS_FLAG_FLOAT(name, env, def, cat, help) { #name, env, float(def), cat, help },
    #define FDS_FLAG_INT(name, env, def, cat, help)
    #include "FeatureFlags.def"
    #undef FDS_FLAG_BOOL
    #undef FDS_FLAG_FLOAT
    #undef FDS_FLAG_INT
};
constexpr IntDef kIntDefs[] = {
    #define FDS_FLAG_BOOL(name, env, def, cat, help)
    #define FDS_FLAG_FLOAT(name, env, def, cat, help)
    #define FDS_FLAG_INT(name, env, def, cat, help) { #name, env, int(def), cat, help },
    #include "FeatureFlags.def"
    #undef FDS_FLAG_BOOL
    #undef FDS_FLAG_FLOAT
    #undef FDS_FLAG_INT
};

constexpr int kNumBool  = int(FeatureFlags::BoolId::Count);
constexpr int kNumFloat = int(FeatureFlags::FloatId::Count);
constexpr int kNumInt   = int(FeatureFlags::IntId::Count);
static_assert(sizeof(kBoolDefs)  / sizeof(kBoolDefs[0])  == kNumBool,  "bool flag count mismatch");
static_assert(sizeof(kFloatDefs) / sizeof(kFloatDefs[0]) == kNumFloat, "float flag count mismatch");
static_assert(sizeof(kIntDefs)   / sizeof(kIntDefs[0])   == kNumInt,   "int flag count mismatch");

struct State {
    std::array<bool,  kNumBool>  boolVals{};
    std::array<float, kNumFloat> floatVals{};
    std::array<int,   kNumInt>   intVals{};
    std::array<bool,  kNumBool>  boolSet{};
    std::array<bool,  kNumFloat> floatSet{};
    std::array<bool,  kNumInt>   intSet{};
};

// Env-var truthiness: existing semantics — "1" is on, anything else off.
// Keeps legacy shell exports working without surprise.
bool envIsTruthy(const char *v) {
    return v && v[0] == '1';
}

// CLI value semantics: lenient — "0/false/no" off, everything else on.
// Bare flag (no =value) is on; --no-flag flips to off.
bool cliBoolValue(const char *v) {
    if (!v || !*v) return true;
    return !(v[0] == '0' || v[0] == 'f' || v[0] == 'F' || v[0] == 'n' || v[0] == 'N');
}

State &state() {
    static State s = []{
        State init;
        for (int i = 0; i < kNumBool;  ++i) init.boolVals[i]  = kBoolDefs[i].defaultValue;
        for (int i = 0; i < kNumFloat; ++i) init.floatVals[i] = kFloatDefs[i].defaultValue;
        for (int i = 0; i < kNumInt;   ++i) init.intVals[i]   = kIntDefs[i].defaultValue;
        // Eager env scan so flag accessors invoked before parseArgs() still
        // see the env overrides — many callers fire from static initialisers.
        for (int i = 0; i < kNumBool; ++i) {
            if (const char *e = std::getenv(kBoolDefs[i].envVar)) {
                init.boolVals[i] = envIsTruthy(e);
                init.boolSet[i]  = true;
            }
        }
        for (int i = 0; i < kNumFloat; ++i) {
            if (const char *e = std::getenv(kFloatDefs[i].envVar)) {
                init.floatVals[i] = float(std::atof(e));
                init.floatSet[i]  = true;
            }
        }
        for (int i = 0; i < kNumInt; ++i) {
            if (const char *e = std::getenv(kIntDefs[i].envVar)) {
                init.intVals[i] = std::atoi(e);
                init.intSet[i]  = true;
            }
        }
        return init;
    }();
    return s;
}

int findBoolByCliName(const char *cli) {
    for (int i = 0; i < kNumBool; ++i)
        if (std::strcmp(kBoolDefs[i].name, cli) == 0) return i;
    return -1;
}
int findFloatByCliName(const char *cli) {
    for (int i = 0; i < kNumFloat; ++i)
        if (std::strcmp(kFloatDefs[i].name, cli) == 0) return i;
    return -1;
}
int findIntByCliName(const char *cli) {
    for (int i = 0; i < kNumInt; ++i)
        if (std::strcmp(kIntDefs[i].name, cli) == 0) return i;
    return -1;
}

// Translate the stored C-identifier form (underscores) to the public
// CLI form (dashes). Buffer is caller-owned. Returns buf.
const char *toCliForm(char *buf, size_t bufLen, const char *name) {
    size_t i = 0;
    for (; name[i] && i + 1 < bufLen; ++i) {
        buf[i] = (name[i] == '_') ? '-' : name[i];
    }
    buf[i] = '\0';
    return buf;
}

void printRow(std::FILE *out, const char *cliName, const char *envVar,
              const char *defaultStr, const char *category, const char *help) {
    char buf[128];
    std::fprintf(out, "  --%-30s %-30s %-10s [%s] %s\n",
                 toCliForm(buf, sizeof(buf), cliName),
                 envVar, defaultStr, category, help);
}

} // namespace

bool FeatureFlags::parseArgs(int argc, const char *const *argv) {
    State &s = state();
    bool helpRequested = false;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!arg || arg[0] != '-' || arg[1] != '-') continue;
        const char *body = arg + 2;
        if (*body == '\0') continue; // bare "--"
        if (std::strcmp(body, "help") == 0 || std::strcmp(body, "h") == 0) {
            helpRequested = true;
            continue;
        }
        bool negate = false;
        const char *name = body;
        if (std::strncmp(name, "no-", 3) == 0) {
            negate = true;
            name += 3;
        }
        const char *eq = std::strchr(name, '=');
        char nameBuf[128];
        const char *value = nullptr;
        // Always copy + dash→underscore so both `--vol-cone-analytic`
        // and `--vol_cone_analytic` resolve to the same registry entry.
        // Stored canonical form is underscore (C identifier), but every
        // user-facing surface emits dashes — see printHelp / completion.
        {
            const size_t nameLen = eq ? size_t(eq - name) : std::strlen(name);
            size_t n = nameLen >= sizeof(nameBuf) ? sizeof(nameBuf) - 1 : nameLen;
            for (size_t i = 0; i < n; ++i) {
                nameBuf[i] = (name[i] == '-') ? '_' : name[i];
            }
            nameBuf[n] = '\0';
            name = nameBuf;
            if (eq) value = eq + 1;
        }
        int bi = findBoolByCliName(name);
        if (bi >= 0) {
            bool v = cliBoolValue(value);
            if (negate) v = !v;
            s.boolVals[bi] = v;
            s.boolSet[bi]  = true;
            continue;
        }
        int fi = findFloatByCliName(name);
        if (fi >= 0) {
            if (!value) {
                std::fprintf(stderr, "flag --%s requires a value\n", name);
                continue;
            }
            s.floatVals[fi] = float(std::atof(value));
            s.floatSet[fi]  = true;
            continue;
        }
        int ii = findIntByCliName(name);
        if (ii >= 0) {
            if (!value) {
                std::fprintf(stderr, "flag --%s requires a value\n", name);
                continue;
            }
            s.intVals[ii] = std::atoi(value);
            s.intSet[ii]  = true;
            continue;
        }
        // Unknown --foo: silently skipped so this parser composes with the
        // snapshot / bench parsers (each owns its own set of long options).
    }
    if (helpRequested) {
        printHelp(stderr);
        return false;
    }
    return true;
}

void FeatureFlags::printHelp(std::FILE *out) {
    std::fprintf(out,
        "FDS feature flags. Three ways to set each, highest precedence first:\n"
        "  1. Command line:  --<flag>          enable bool, or:\n"
        "                    --no-<flag>       disable bool\n"
        "                    --<flag>=VALUE    set value (or explicit bool, 0/1/true/false/no)\n"
        "  2. Environment:   the listed env var; \"1\" is on, anything else off.\n"
        "  3. Compile-time:  the default shown below (WASM flips a few via -D).\n\n");
    // Print by category, in table order. Walk both bool + value lists once
    // per distinct category we see, in order of first appearance.
    auto printedCategory = [](const char *cat, const char **seen, int seenCount) {
        for (int i = 0; i < seenCount; ++i)
            if (std::strcmp(seen[i], cat) == 0) return true;
        return false;
    };
    const char *seen[64];
    int seenCount = 0;
    auto walk = [&](const char *cat) {
        std::fprintf(out, "[%s]\n", cat);
        for (int i = 0; i < kNumBool; ++i) {
            if (std::strcmp(kBoolDefs[i].category, cat) != 0) continue;
            printRow(out, kBoolDefs[i].name, kBoolDefs[i].envVar,
                     kBoolDefs[i].defaultValue ? "on" : "off",
                     kBoolDefs[i].category, kBoolDefs[i].help);
        }
        for (int i = 0; i < kNumFloat; ++i) {
            if (std::strcmp(kFloatDefs[i].category, cat) != 0) continue;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", kFloatDefs[i].defaultValue);
            printRow(out, kFloatDefs[i].name, kFloatDefs[i].envVar, buf,
                     kFloatDefs[i].category, kFloatDefs[i].help);
        }
        for (int i = 0; i < kNumInt; ++i) {
            if (std::strcmp(kIntDefs[i].category, cat) != 0) continue;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%d", kIntDefs[i].defaultValue);
            printRow(out, kIntDefs[i].name, kIntDefs[i].envVar, buf,
                     kIntDefs[i].category, kIntDefs[i].help);
        }
        std::fputc('\n', out);
    };
    for (int i = 0; i < kNumBool; ++i) {
        const char *c = kBoolDefs[i].category;
        if (printedCategory(c, seen, seenCount)) continue;
        if (seenCount < int(sizeof(seen)/sizeof(seen[0]))) seen[seenCount++] = c;
        walk(c);
    }
    for (int i = 0; i < kNumFloat; ++i) {
        const char *c = kFloatDefs[i].category;
        if (printedCategory(c, seen, seenCount)) continue;
        if (seenCount < int(sizeof(seen)/sizeof(seen[0]))) seen[seenCount++] = c;
        walk(c);
    }
    for (int i = 0; i < kNumInt; ++i) {
        const char *c = kIntDefs[i].category;
        if (printedCategory(c, seen, seenCount)) continue;
        if (seenCount < int(sizeof(seen)/sizeof(seen[0]))) seen[seenCount++] = c;
        walk(c);
    }
}

void FeatureFlags::printActive(std::FILE *out) {
    const State &s = state();
    bool any = false;
    char buf[128];
    for (int i = 0; i < kNumBool; ++i) {
        if (!s.boolSet[i]) continue;
        if (!any) { std::fprintf(out, "Active flags:\n"); any = true; }
        std::fprintf(out, "  %s = %s\n",
                     toCliForm(buf, sizeof(buf), kBoolDefs[i].name),
                     s.boolVals[i] ? "on" : "off");
    }
    for (int i = 0; i < kNumFloat; ++i) {
        if (!s.floatSet[i]) continue;
        if (!any) { std::fprintf(out, "Active flags:\n"); any = true; }
        std::fprintf(out, "  %s = %g\n",
                     toCliForm(buf, sizeof(buf), kFloatDefs[i].name),
                     s.floatVals[i]);
    }
    for (int i = 0; i < kNumInt; ++i) {
        if (!s.intSet[i]) continue;
        if (!any) { std::fprintf(out, "Active flags:\n"); any = true; }
        std::fprintf(out, "  %s = %d\n",
                     toCliForm(buf, sizeof(buf), kIntDefs[i].name),
                     s.intVals[i]);
    }
}

void FeatureFlags::printCompletion(std::FILE *out, const char *shell) {
    const bool zsh = shell && (shell[0] == 'z' || shell[0] == 'Z');
    // Emit one --flag form for every registry entry, plus --no-<flag>
    // for bools. Static snapshot scenes / bench kinds / standard switches
    // are included so the user gets one-stop completion.
    std::fprintf(out, "# REVIVAL/FLOOD DEMO shell completion (%s)\n", zsh ? "zsh" : "bash");
    std::fprintf(out, "# Generated by ./DEMO --print-completion=%s\n",  zsh ? "zsh" : "bash");
    std::fprintf(out, "# Usage:  source <(./DEMO --print-completion=%s)\n\n", zsh ? "zsh" : "bash");

    // Build the word list once. Emit dash form (the underscore form is
    // accepted too at parse time, but the public/preferred form is dashes).
    std::fprintf(out, "_demo_flags=(\n");
    std::fprintf(out, "  --help -h --print-completion=bash --print-completion=zsh\n");
    std::fprintf(out, "  --snapshot= --bench= --out=\n");
    char buf[128];
    for (int i = 0; i < kNumBool; ++i) {
        const char *n = toCliForm(buf, sizeof(buf), kBoolDefs[i].name);
        std::fprintf(out, "  --%s --no-%s\n", n, n);
    }
    for (int i = 0; i < kNumFloat; ++i) {
        std::fprintf(out, "  --%s=\n", toCliForm(buf, sizeof(buf), kFloatDefs[i].name));
    }
    for (int i = 0; i < kNumInt; ++i) {
        std::fprintf(out, "  --%s=\n", toCliForm(buf, sizeof(buf), kIntDefs[i].name));
    }
    std::fprintf(out, ")\n\n");

    if (zsh) {
        // zsh: simple compdef using a function that just calls compadd.
        std::fprintf(out,
            "_demo() { compadd -- \"${_demo_flags[@]}\"; }\n"
            "compdef _demo DEMO ./DEMO\n");
    } else {
        // bash: complete -F handler. compopt -o nospace lets --flag= stay
        // attached to its value without a separator.
        std::fprintf(out,
            "_demo() {\n"
            "  local cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
            "  COMPREPLY=( $(compgen -W \"${_demo_flags[*]}\" -- \"$cur\") )\n"
            "  if [[ \"$cur\" == *= ]]; then compopt -o nospace 2>/dev/null; fi\n"
            "  return 0\n"
            "}\n"
            "complete -F _demo DEMO ./DEMO\n");
    }
}

bool  FeatureFlags::get(BoolId  id) { return state().boolVals[int(id)]; }
float FeatureFlags::get(FloatId id) { return state().floatVals[int(id)]; }
int   FeatureFlags::get(IntId   id) { return state().intVals[int(id)]; }
bool  FeatureFlags::isSet(BoolId  id) { return state().boolSet[int(id)]; }
bool  FeatureFlags::isSet(FloatId id) { return state().floatSet[int(id)]; }
bool  FeatureFlags::isSet(IntId   id) { return state().intSet[int(id)]; }

} // namespace fds
