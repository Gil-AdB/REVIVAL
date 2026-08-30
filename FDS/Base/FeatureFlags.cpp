#include "FeatureFlags.h"

#include <array>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#ifdef _WIN32
#include "WinCompat.h"   // realpath(), PATH_MAX
#endif

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

// Tokens the OTHER argv parsers own (DEMO/REV.CPP pre-scans, Snapshot.cpp's
// ParseSnapshotArgs/ParseBenchArgs, MaterialImport_ParseArgs). They flow
// through parseArgs too, so the unknown-flag warning must not fire on them.
// Names are compared AFTER dash→underscore normalization and after the
// `=value` split, so exact matching is enough (e.g. `--snapshot=city@t=1`
// arrives here as name "snapshot").
bool ownedByOtherParser(const char *name) {
    static const char *const kKnownElsewhere[] = {
        "snapshot",                    // DEMO/Snapshot.cpp ParseSnapshotArgs + REV.CPP headless scan
        "out",                         // DEMO/Snapshot.cpp ParseSnapshotArgs
        "bench",                       // DEMO/Snapshot.cpp ParseBenchArgs + REV.CPP headless scan
        "repro",                       // DEMO/ReproHarness.cpp ParseReproArgs + REV.CPP headless scan
                                       // (--repro=<scene>@t=... carries a scene+time payload, so it is
                                       //  not a FeatureFlag; without this entry --strict_flags — ON by
                                       //  default since 1782351 — killed every command line in
                                       //  docs/INTERACTIVE_REPRO.md.)
        "material_import",             // DEMO/MaterialImport.cpp MaterialImport_ParseArgs
        "material_import_flip_normal",
        "material_import_no_mips",
        "print_completion",            // DEMO/REV.CPP (exits before parseArgs, listed for safety)
        "pcl_dump",                    // DEMO/FOUNTAIN.CPP ParticleDump_ParseArgs (carries a PATH)
    };
    for (const char *k : kKnownElsewhere)
        if (std::strcmp(name, k) == 0) return true;
    return false;
}

bool applyOneToken(State &s, const char *arg);   // fwd (flags-file recursion)

// True only while parseArgs walks argv. --strict_flags kills the run on an
// unknown token there and NOWHERE else: a flags-file line, FDS_BAKED_ARGS, a
// SCRIPTS/*.params entry and the live tune console all reach applyOneToken too,
// and aborting a running demo over a stale script line would be worse than the
// typo. Not thread-safe by design — argv parsing is single-threaded at startup.
bool g_inArgvParse = false;

// ── --vanilla / FDS_VANILLA=1 ───────────────────────────────────────────
// A true BARE BASELINE: every flag back to its compile-time default AND
// marked explicitly-set. The set marks are the whole point — without them
// the scene drivers' FeatureFlags::setDefault blocks (greets' PBR/HDR/
// shadow look, chase's bolt-light reach, crash's exposure) and the
// SCRIPTS/<scene>.params lines would re-enable everything a frame later
// and "vanilla" would be a lie.
//
// ORDERING is pure parse order, exactly like --flags-file: applyVanilla
// erases whatever came before it and anything named after it wins, so
// `--vanilla --deferred --pbr` = bare + those two. That covers the CLI,
// a `vanilla` line inside a --flags-file, and FDS_BAKED_ARGS, since all
// three funnel through applyOneToken.
//
// ENV: the eager FDS_* scan in state() runs BEFORE argv, so a --vanilla on
// the command line clears env-set values as a side effect of the reset —
// intended ("turn everything off" means everything). The env FORM
// (FDS_VANILLA=1) has no ordering of its own, so state() applies it AFTER
// the env scan to keep the same rule: env counts as "before".
//
// NOT "everything off": flags whose compile default is ON stay on
// (param_scripts, tune_server, warn_unknown_flags, deferred_zcull,
// metal_map/roughness_map/ao_map, ...). `deferred` defaults off, so a
// vanilla run renders the FORWARD path — a legitimate mode, not a crash.
void applyVanilla(State &s) {
    for (int i = 0; i < kNumBool;  ++i) { s.boolVals[i]  = kBoolDefs[i].defaultValue;  s.boolSet[i]  = true; }
    for (int i = 0; i < kNumFloat; ++i) { s.floatVals[i] = kFloatDefs[i].defaultValue; s.floatSet[i] = true; }
    for (int i = 0; i < kNumInt;   ++i) { s.intVals[i]   = kIntDefs[i].defaultValue;   s.intSet[i]   = true; }
    // Keep the state self-describing: the reset above put `vanilla` itself
    // back to its own compile default (off), which would make a vanilla run
    // report that it is not one.
    const int vi = findBoolByCliName("vanilla");
    if (vi >= 0) s.boolVals[vi] = true;
    std::fprintf(stderr,
        "[FLAGS] --vanilla: %d flags forced to compile-time defaults and marked "
        "explicitly-set (scene setDefault() + SCRIPTS/*.params suppressed; env-set "
        "values cleared). Tokens named AFTER it still win.\n",
        kNumBool + kNumFloat + kNumInt);
}

// ── --flags-file=<path> ─────────────────────────────────────────────────
// Reads a text file of flag tokens, one per line, and applies each through
// applyOneToken in file order. Canonical line form is the bare token WITHOUT
// the `--` prefix (`hdr`, `no-bloom`, `fast_fog_density=30`); a leading `--`
// is accepted too so a CLI line can be pasted verbatim. `#` comments and
// blank lines are skipped; leading/trailing whitespace is trimmed; ONE token
// per line (no intra-line splitting, so values may contain spaces). A line
// may itself be `flags-file=<other>` — nesting is capped at kFlagsFileMaxDepth
// and include cycles are detected (both log and skip). Precedence is pure
// parse order: a `--flag` AFTER `--flags-file=` on the command line overrides
// the file's value; a flag INSIDE the file overrides tokens before the
// include point. Paths are relative to the CWD (Runtime/ at demo runtime).
constexpr int kFlagsFileMaxDepth = 4;
int         g_flagsFileDepth = 0;
std::string g_flagsFileStack[kFlagsFileMaxDepth];

void applyFlagsFile(State &s, const char *path) {
    if (g_flagsFileDepth >= kFlagsFileMaxDepth) {
        std::fprintf(stderr, "[FLAGS] flags-file '%s': include depth cap (%d) hit — skipped\n",
                     path, kFlagsFileMaxDepth);
        return;
    }
    // Cycle check on the canonicalized path (falls back to the raw path when
    // realpath fails — e.g. dangling relative path; fopen will complain next).
    char realBuf[PATH_MAX];
    const char *key = realpath(path, realBuf) ? realBuf : path;
    for (int i = 0; i < g_flagsFileDepth; ++i) {
        if (g_flagsFileStack[i] == key) {
            std::fprintf(stderr, "[FLAGS] flags-file '%s': include cycle — skipped\n", path);
            return;
        }
    }
    std::FILE *f = std::fopen(path, "r");
    if (!f) {
        std::fprintf(stderr, "[FLAGS] cannot open flags file '%s'\n", path);
        return;
    }
    g_flagsFileStack[g_flagsFileDepth++] = key;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        line[std::strcspn(line, "\r\n")] = 0;
        char *tok = line;
        while (*tok == ' ' || *tok == '\t') ++tok;
        for (char *e = tok + std::strlen(tok); e > tok && (e[-1] == ' ' || e[-1] == '\t'); )
            *--e = 0;
        if (!tok[0] || tok[0] == '#') continue;
        char withDashes[560];
        if (!(tok[0] == '-' && tok[1] == '-')) {
            std::snprintf(withDashes, sizeof withDashes, "--%s", tok);
            tok = withDashes;
        }
        applyOneToken(s, tok);   // --help inside a file is ignored by design
    }
    --g_flagsFileDepth;
    std::fclose(f);
}

// Apply a single `--foo` / `--foo=value` / `--no-foo` token to the State.
// Returns true if the token requested --help / -h; the public parseArgs
// uses that to forward to printHelp. `--flags-file=<path>` is handled here
// (see applyFlagsFile above) so it works on the CLI, in FDS_BAKED_ARGS and
// nested inside another flags file. Unknown tokens are skipped so this
// parser composes with the snapshot / bench parsers — but tokens no parser
// owns now warn on stderr (suppress via --no-warn-unknown-flags / env).
bool applyOneToken(State &s, const char *arg) {
    if (!arg || arg[0] != '-' || arg[1] != '-') return false;
    const char *body = arg + 2;
    if (*body == '\0') return false; // bare "--"
    if (std::strcmp(body, "help") == 0 || std::strcmp(body, "h") == 0) return true;
    const char *name = body;
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
    // Flags-file include — not a registry entry (it's an action, not state).
    if (std::strcmp(name, "flags_file") == 0) {
        if (!value || !*value) {
            std::fprintf(stderr, "flag --flags-file requires a value (path)\n");
            return false;
        }
        applyFlagsFile(s, value);
        return false;
    }
    // Lookup-first / strip-no-second: handle ambiguity between
    // `--no-foo` as the negation of `foo`, and `--no-foo` as the
    // literal flag `no_foo`. Direct lookup wins so `--no-sort` /
    // `--no-greets-spots` find their literally-named flags before
    // the strip-prefix path runs.
    bool negate = false;
    int bi = findBoolByCliName(name);
    if (bi < 0 && std::strncmp(name, "no_", 3) == 0) {
        negate = true;
        name += 3;
        bi = findBoolByCliName(name);
    }
    if (bi >= 0) {
        bool v = cliBoolValue(value);
        if (negate) v = !v;
        // --vanilla is an ACTION, not a stored value (see applyVanilla): it
        // rewrites the whole State in place, here in parse order, so later
        // tokens override it. --no-vanilla / --vanilla=0 falls through to the
        // plain store below and resets nothing.
        if (v && std::strcmp(kBoolDefs[bi].name, "vanilla") == 0) {
            applyVanilla(s);
            return false;
        }
        s.boolVals[bi] = v;
        s.boolSet[bi]  = true;
        return false;
    }
    int fi = findFloatByCliName(name);
    if (fi >= 0) {
        if (!value) {
            std::fprintf(stderr, "flag --%s requires a value\n", name);
            return false;
        }
        s.floatVals[fi] = float(std::atof(value));
        s.floatSet[fi]  = true;
        return false;
    }
    int ii = findIntByCliName(name);
    if (ii >= 0) {
        if (!value) {
            std::fprintf(stderr, "flag --%s requires a value\n", name);
            return false;
        }
        s.intVals[ii] = std::atoi(value);
        s.intSet[ii]  = true;
        return false;
    }
    // Unknown --foo: skipped. Warn (once per token instance) unless the token
    // belongs to one of the other argv parsers or warnings are suppressed —
    // an unknown flag used to vanish silently, which turned a typo like
    // `--fast-fog-blob` (for fast_fog_blobs) into a hard-to-spot no-op.
    // Read the warn flag straight from the State (the g_* accessor globals
    // aren't bound yet when this runs from the state() init lambda for
    // FDS_BAKED_ARGS).
    static const int kWarnIdx = findBoolByCliName("warn_unknown_flags");
    const bool foreign = ownedByOtherParser(name);
    if (kWarnIdx >= 0 && s.boolVals[kWarnIdx] && !foreign) {
        std::fprintf(stderr, "[FLAGS] unknown flag '%s' (ignored)\n", arg);
    }
    // --strict_flags (default ON): an unknown ARGV token kills the run. An
    // ignored flag renders a plausible image that answers a DIFFERENT question,
    // and that silent false negative has cost this project days — most often via
    // zsh, which does NOT word-split unquoted expansions, so `./DEMO $ARM` hands
    // the whole flag string over as ONE token and every flag is dropped. Argv
    // only: a flags-file line, FDS_BAKED_ARGS, a .params entry or a live tune
    // edit still merely warns, because killing a running demo over a stale
    // script line is worse than the typo it reports.
    static const int kStrictIdx = findBoolByCliName("strict_flags");
    if (g_inArgvParse && !foreign && kStrictIdx >= 0 && s.boolVals[kStrictIdx]) {
        std::fprintf(stderr,
            "[FLAGS] FATAL: unknown flag '%s'.\n"
            "        If this looks like a whole command line in one token, the shell\n"
            "        did not split it: zsh does not word-split unquoted expansions.\n"
            "        Pass flags as separate argv words, or use --no-strict_flags.\n",
            arg);
        std::exit(2);
    }
    return false;
}

// Tokenize a space/tab-separated string in-place and feed each token through
// applyOneToken. Buffer must be writable; tokens are NUL-terminated as the
// scan advances. Used for the FDS_BAKED_ARGS compile-time-baked CLI string
// (mainly for wasm builds where getenv / argv aren't routinely available),
// and reusable for any future "apply a CLI string" caller.
void applyTokenString(State &s, char *buf) {
    char *p = buf;
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        char *tok = p;
        while (*p && *p != ' ' && *p != '\t') ++p;
        if (*p) *p++ = '\0';
        applyOneToken(s, tok);
    }
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
        // FDS_VANILLA=1 — the env form of --vanilla. Applied AFTER the env
        // scan above so it clears env-set values too (the CLI form clears
        // them by construction, since argv is parsed after this whole
        // lambda); FDS_BAKED_ARGS below and parseArgs later still win, which
        // keeps the single rule "anything named after --vanilla wins".
        {
            const int vi = findBoolByCliName("vanilla");
            if (vi >= 0 && envIsTruthy(std::getenv(kBoolDefs[vi].envVar)))
                applyVanilla(init);
        }
        // Compile-time-baked CLI string (mainly for wasm builds where
        // argv and getenv aren't routinely available). Set at CMake
        // configure time via -DFDS_BAKED_ARGS="..." or the env var of
        // the same name; runs through the same parser as parseArgs so
        // anything valid on the CLI works here. Runtime parseArgs runs
        // AFTER this and wins on conflicts.
#ifdef FDS_BAKED_ARGS
        {
            static char kBaked[] = FDS_BAKED_ARGS;
            applyTokenString(init, kBaked);
        }
#endif
        return init;
    }();
    return s;
}
} // namespace

FeatureFlags::ParamRef FeatureFlags::findParam(const char *name) {
    char buf[128];
    size_t n = 0;
    for (; name[n] && n + 1 < sizeof(buf); ++n)
        buf[n] = (name[n] == '-') ? '_' : name[n];
    buf[n] = '\0';
    int i = findBoolByCliName(buf);
    if (i >= 0) return { ParamType::Bool, i };
    i = findFloatByCliName(buf);
    if (i >= 0) return { ParamType::Float, i };
    i = findIntByCliName(buf);
    if (i >= 0) return { ParamType::Int, i };
    return {};
}

bool FeatureFlags::parseArgs(int argc, const char *const *argv) {
    State &s = state();
    bool helpRequested = false;
    g_inArgvParse = true;
    for (int i = 1; i < argc; ++i) {
        if (applyOneToken(s, argv[i])) helpRequested = true;
    }
    g_inArgvParse = false;
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
        "  3. Compile-time:  the default shown below (WASM flips a few via -D).\n"
        "\n"
        "  --flags-file=FILE loads flag tokens from FILE (path relative to the\n"
        "  CWD, i.e. Runtime/), one per line. Canonical line form is the token\n"
        "  without the `--` prefix (`hdr`, `no-bloom`, `fast_fog_density=30`);\n"
        "  a leading `--` is accepted too. `#` comments / blank lines skipped.\n"
        "  Precedence is parse order = position on the command line: tokens\n"
        "  AFTER --flags-file= override the file, tokens inside the file\n"
        "  override what came before it. Files can include other files via a\n"
        "  flags-file= line (depth capped at 4, cycles detected).\n"
        "  Starter preset: --flags-file=PRESETS/city-noir.flags\n\n"
        "  --vanilla       BARE BASELINE — every flag to its compile-time default,\n"
        "  marked explicitly-set so the SCENE defaults (greets' PBR/HDR block, ...)\n"
        "  and SCRIPTS/*.params are suppressed too. PUT IT FIRST: parse order wins,\n"
        "  so `--vanilla --deferred --pbr` = bare + those two. Clears env-set values\n"
        "  as well (FDS_VANILLA=1 is the env form). Compile DEFAULTS, not all-off —\n"
        "  `deferred` defaults off, so a vanilla run renders the FORWARD path.\n\n");
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
    std::fprintf(out, "  --snapshot= --bench= --out= --flags-file=\n");
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

// Bind the inline-accessor pointer globals to the singleton State's
// arrays. Initialized at namespace-scope dynamic-init time, AFTER the
// State lambda completes (same TU, declared after state() above).
// The cross-TU init-order trap doesn't bite because all current
// callers of FeatureFlags::get() are either function-scope (lazy,
// called after main()) or member-function bodies (called per render
// pass, well after dynamic init). No file-scope static initializer
// elsewhere reads from these — verified 2026-05-31.
bool  * const FeatureFlags::g_boolVals  = state().boolVals.data();
float * const FeatureFlags::g_floatVals = state().floatVals.data();
int   * const FeatureFlags::g_intVals   = state().intVals.data();
bool  * const FeatureFlags::g_boolSet   = state().boolSet.data();
bool  * const FeatureFlags::g_floatSet  = state().floatSet.data();
bool  * const FeatureFlags::g_intSet    = state().intSet.data();


// ── Tune-server support ─────────────────────────────────────────────────

static void jsonEscapeInto(std::string &out, const char *s) {
    for (; *s; ++s) {
        const char c = *s;
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((unsigned char)c < 0x20) { out += ' '; }
        else out += c;
    }
}

void FeatureFlags::dumpParamsJson(std::string &out) {
    char buf[64];
    out += "[";
    bool first = true;
    auto emit = [&](const char *name, const char *type, const char *cat,
                    const char *help, const char *val, const char *def,
                    bool set) {
        if (!first) out += ",";
        first = false;
        out += "{\"name\":\"";   out += name;
        out += "\",\"type\":\""; out += type;
        out += "\",\"cat\":\"";  jsonEscapeInto(out, cat);
        out += "\",\"help\":\""; jsonEscapeInto(out, help);
        out += "\",\"value\":";  out += val;
        out += ",\"def\":";       out += def;
        out += ",\"set\":";       out += set ? "true" : "false";
        out += "}";
    };
    for (int i = 0; i < kNumBool; ++i) {
        emit(kBoolDefs[i].name, "bool", kBoolDefs[i].category, kBoolDefs[i].help,
             g_boolVals[i] ? "true" : "false",
             kBoolDefs[i].defaultValue ? "true" : "false", g_boolSet[i]);
    }
    for (int i = 0; i < kNumFloat; ++i) {
        char v[48], d[48];
        std::snprintf(v, sizeof v, "%g", g_floatVals[i]);
        std::snprintf(d, sizeof d, "%g", kFloatDefs[i].defaultValue);
        emit(kFloatDefs[i].name, "float", kFloatDefs[i].category, kFloatDefs[i].help,
             v, d, g_floatSet[i]);
    }
    for (int i = 0; i < kNumInt; ++i) {
        char v[32], d[32];
        std::snprintf(v, sizeof v, "%d", g_intVals[i]);
        std::snprintf(d, sizeof d, "%d", kIntDefs[i].defaultValue);
        emit(kIntDefs[i].name, "int", kIntDefs[i].category, kIntDefs[i].help,
             v, d, g_intSet[i]);
    }
    out += "]";
    (void)buf;
}

void FeatureFlags::dumpProvenanceFlagsJson(std::string &out) {
    out += "[";
    bool first = true;
    auto emit = [&](const char *name, const char *type, const char *val,
                    const char *def, bool set) {
        if (!first) out += ",";
        first = false;
        out += "{\"name\":\"";     out += name;
        out += "\",\"type\":\"";   out += type;
        out += "\",\"value\":";    out += val;
        out += ",\"default\":";    out += def;
        out += ",\"explicit\":";   out += set ? "true" : "false";
        out += "}";
    };
    for (int i = 0; i < kNumBool; ++i) {
        const bool differs = (g_boolVals[i] != kBoolDefs[i].defaultValue);
        if (!differs && !g_boolSet[i]) continue;
        emit(kBoolDefs[i].name, "bool",
             g_boolVals[i] ? "true" : "false",
             kBoolDefs[i].defaultValue ? "true" : "false", g_boolSet[i]);
    }
    for (int i = 0; i < kNumFloat; ++i) {
        // Bitwise compare: a float knob nudged to a value that PRINTS the same
        // as the default is still a different render, and provenance that hides
        // that is worse than none.
        const bool differs = std::memcmp(&g_floatVals[i], &kFloatDefs[i].defaultValue,
                                         sizeof(float)) != 0;
        if (!differs && !g_floatSet[i]) continue;
        char v[48], d[48];
        std::snprintf(v, sizeof v, "%.9g", (double)g_floatVals[i]);
        std::snprintf(d, sizeof d, "%.9g", (double)kFloatDefs[i].defaultValue);
        emit(kFloatDefs[i].name, "float", v, d, g_floatSet[i]);
    }
    for (int i = 0; i < kNumInt; ++i) {
        const bool differs = (g_intVals[i] != kIntDefs[i].defaultValue);
        if (!differs && !g_intSet[i]) continue;
        char v[32], d[32];
        std::snprintf(v, sizeof v, "%d", g_intVals[i]);
        std::snprintf(d, sizeof d, "%d", kIntDefs[i].defaultValue);
        emit(kIntDefs[i].name, "int", v, d, g_intSet[i]);
    }
    out += "]";
}

bool FeatureFlags::setParamFromText(const char *name, const char *value) {
    const ParamRef r = findParam(name);
    // --vanilla is a startup-only ACTION (see applyVanilla). Applying it live
    // would contradict everything already built from the init-time flags
    // (scene geometry, shadow/env/lightmap bakes, G-buffer layout), so the
    // tune console gets an honest 400 instead of a knob that lies.
    if (r.type == ParamType::Bool && r.index == findBoolByCliName("vanilla")) {
        std::fprintf(stderr, "[FLAGS] 'vanilla' is startup-only — relaunch with --vanilla\n");
        return false;
    }
    switch (r.type) {
    case ParamType::Bool: {
        const bool on = !(std::strcmp(value, "0") == 0 ||
                          std::strcmp(value, "false") == 0 ||
                          std::strcmp(value, "off") == 0);
        g_boolVals[r.index] = on;
        g_boolSet[r.index]  = true;
        return true;
    }
    case ParamType::Float: {
        char *end = nullptr;
        const float v = std::strtof(value, &end);
        if (end == value) return false;
        g_floatVals[r.index] = v;
        g_floatSet[r.index]  = true;
        return true;
    }
    case ParamType::Int: {
        char *end = nullptr;
        const long v = std::strtol(value, &end, 10);
        if (end == value) return false;
        g_intVals[r.index] = int(v);
        g_intSet[r.index]  = true;
        return true;
    }
    default: return false;
    }
}

void FeatureFlags::dumpSetParams(std::vector<std::pair<std::string, std::string>> &out) {
    char b[48];
    for (int i = 0; i < kNumBool; ++i)
        if (g_boolSet[i]) out.emplace_back(kBoolDefs[i].name, g_boolVals[i] ? "1" : "0");
    for (int i = 0; i < kNumFloat; ++i)
        if (g_floatSet[i]) {
            std::snprintf(b, sizeof b, "%g", g_floatVals[i]);
            out.emplace_back(kFloatDefs[i].name, b);
        }
    for (int i = 0; i < kNumInt; ++i)
        if (g_intSet[i]) {
            std::snprintf(b, sizeof b, "%d", g_intVals[i]);
            out.emplace_back(kIntDefs[i].name, b);
        }
}

bool FeatureFlags::clearSetMark(const char *name) {
    const ParamRef r = findParam(name);
    switch (r.type) {
    case ParamType::Bool:  g_boolSet[r.index]  = false; return true;
    case ParamType::Float: g_floatSet[r.index] = false; return true;
    case ParamType::Int:   g_intSet[r.index]   = false; return true;
    default: return false;
    }
}

bool FeatureFlags::unsetParam(const char *name) {
    const ParamRef r = findParam(name);
    switch (r.type) {
    case ParamType::Bool:
        g_boolVals[r.index] = kBoolDefs[r.index].defaultValue;
        g_boolSet[r.index]  = false;
        return true;
    case ParamType::Float:
        g_floatVals[r.index] = kFloatDefs[r.index].defaultValue;
        g_floatSet[r.index]  = false;
        return true;
    case ParamType::Int:
        g_intVals[r.index] = kIntDefs[r.index].defaultValue;
        g_intSet[r.index]  = false;
        return true;
    default: return false;
    }
}

} // namespace fds
