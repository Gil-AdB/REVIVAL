#include "ParamScript.h"
#include "FeatureFlags.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace fds {
namespace {

using PT = FeatureFlags::ParamType;

struct Key {
	float t;
	float v;
};

struct Track {
	FeatureFlags::ParamRef ref;
	std::string            name;       // for messages
	std::vector<Key>       keys;       // sorted by t; size 1 = constant
	bool                   shadowed;   // CLI/env set this flag — script skipped
};

struct Saved {
	FeatureFlags::ParamRef ref;
	float                  orig;       // value before the script touched it
};

struct ScriptState {
	std::string        scene;          // active scene name ("" = none)
	std::string        path;           // SCRIPTS/<scene>.params
	std::vector<Track> tracks;
	std::vector<Saved> saved;
	time_t             mtime    = 0;   // 0 = no file loaded
	int                pollSkip = 0;   // frames until next stat()
};

ScriptState g;

float readParam(const FeatureFlags::ParamRef &r) {
	switch (r.type) {
	case PT::Bool:  return FeatureFlags::g_boolVals[r.index] ? 1.0f : 0.0f;
	case PT::Float: return FeatureFlags::g_floatVals[r.index];
	case PT::Int:   return float(FeatureFlags::g_intVals[r.index]);
	default:        return 0.0f;
	}
}

void writeParam(const FeatureFlags::ParamRef &r, float v) {
	switch (r.type) {
	case PT::Bool:  FeatureFlags::g_boolVals[r.index]  = (v >= 0.5f); break;
	case PT::Float: FeatureFlags::g_floatVals[r.index] = v; break;
	case PT::Int:   FeatureFlags::g_intVals[r.index]   = int(std::lround(v)); break;
	default: break;
	}
}

bool paramIsCliSet(const FeatureFlags::ParamRef &r) {
	switch (r.type) {
	case PT::Bool:  return FeatureFlags::g_boolSet[r.index];
	case PT::Float: return FeatureFlags::g_floatSet[r.index];
	case PT::Int:   return FeatureFlags::g_intSet[r.index];
	default:        return false;
	}
}

// Restore every value the current script wrote, then drop the script.
void unloadScript() {
	for (const Saved &s : g.saved) writeParam(s.ref, s.orig);
	g.saved.clear();
	g.tracks.clear();
	g.mtime = 0;
}

char *trim(char *s) {
	while (*s == ' ' || *s == '\t') ++s;
	char *e = s + std::strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
		*--e = '\0';
	return s;
}

// Parse "value" for any param type into a float. Bools accept 0/1/true/
// false/on/off (same leniency as the CLI); numbers pass through.
bool parseValue(const char *v, float &out) {
	if (!*v) return false;
	if (std::strcmp(v, "true") == 0 || std::strcmp(v, "on") == 0)  { out = 1.0f; return true; }
	if (std::strcmp(v, "false") == 0 || std::strcmp(v, "off") == 0) { out = 0.0f; return true; }
	char *end = nullptr;
	out = std::strtof(v, &end);
	return end && *trim(end) == '\0';
}

void loadScript(time_t mtime) {
	unloadScript();
	std::FILE *f = std::fopen(g.path.c_str(), "rb");
	if (!f) return;
	g.mtime = mtime;

	char line[512];
	int lineNo = 0, nKeys = 0, nShadowed = 0;
	while (std::fgets(line, sizeof(line), f)) {
		++lineNo;
		if (char *hash = std::strchr(line, '#')) *hash = '\0';
		char *s = trim(line);
		if (!*s) continue;

		// name [@ time] = value
		char *eq = std::strchr(s, '=');
		if (!eq) {
			std::fprintf(stderr, "[SCRIPT] %s:%d: no '=' — ignored\n",
			             g.path.c_str(), lineNo);
			continue;
		}
		*eq = '\0';
		char *lhs = trim(s);
		char *rhs = trim(eq + 1);

		float keyT = 0.0f;
		bool  hasKey = false;
		if (char *at = std::strchr(lhs, '@')) {
			*at = '\0';
			char *tstr = trim(at + 1);
			char *end = nullptr;
			keyT = std::strtof(tstr, &end);
			if (!end || *trim(end) != '\0') {
				std::fprintf(stderr, "[SCRIPT] %s:%d: bad time '%s' — ignored\n",
				             g.path.c_str(), lineNo, tstr);
				continue;
			}
			hasKey = true;
			lhs = trim(lhs);
		}

		const FeatureFlags::ParamRef ref = FeatureFlags::findParam(lhs);
		if (ref.type == PT::None) {
			std::fprintf(stderr, "[SCRIPT] %s:%d: unknown param '%s' — ignored\n",
			             g.path.c_str(), lineNo, lhs);
			continue;
		}
		float val;
		if (!parseValue(rhs, val)) {
			std::fprintf(stderr, "[SCRIPT] %s:%d: bad value '%s' — ignored\n",
			             g.path.c_str(), lineNo, rhs);
			continue;
		}

		// Find or create the track for this param.
		Track *tr = nullptr;
		for (Track &t : g.tracks)
			if (t.ref.type == ref.type && t.ref.index == ref.index) { tr = &t; break; }
		if (!tr) {
			const bool shadowed = paramIsCliSet(ref);
			if (shadowed) {
				std::fprintf(stderr,
				    "[SCRIPT] %s: '%s' set on CLI/env — script line(s) ignored\n",
				    g.path.c_str(), lhs);
				++nShadowed;
			} else {
				g.saved.push_back({ ref, readParam(ref) });
			}
			g.tracks.push_back({ ref, lhs, {}, shadowed });
			tr = &g.tracks.back();
		}
		// Constant lines are a single key at t=0; keyed lines insert sorted.
		const Key k{ hasKey ? keyT : 0.0f, val };
		auto it = tr->keys.begin();
		while (it != tr->keys.end() && it->t <= k.t) ++it;
		tr->keys.insert(it, k);
		++nKeys;
	}
	std::fclose(f);
	std::fprintf(stderr, "[SCRIPT] loaded %s: %zu params, %d keys%s\n",
	             g.path.c_str(), g.tracks.size(), nKeys,
	             nShadowed ? " (some CLI-shadowed)" : "");
}

float evalTrack(const Track &t, float timer) {
	const std::vector<Key> &k = t.keys;
	if (k.size() == 1 || timer <= k.front().t) return k.front().v;
	if (timer >= k.back().t) return k.back().v;
	size_t i = 1;
	while (i < k.size() && k[i].t < timer) ++i;
	const Key &a = k[i - 1], &b = k[i];
	if (t.ref.type == PT::Bool) return a.v;            // bools step at keys
	const float span = b.t - a.t;
	const float u = span > 0.0f ? (timer - a.t) / span : 1.0f;
	return a.v + (b.v - a.v) * u;                       // floats/ints lerp
}

} // namespace

void ParamScript_SetScene(const char *sceneName) {
	if (!FeatureFlags::param_scripts()) return;
	if (g.scene == sceneName) return;
	unloadScript();
	g.scene = sceneName;
	g.path  = std::string("SCRIPTS/") + sceneName + ".params";
	g.pollSkip = 0;            // stat on the next tick
}

void ParamScript_Tick(float timer) {
	if (!FeatureFlags::param_scripts()) return;
	if (g.scene.empty()) return;

	// Hot-reload poll: stat() every ~15 frames (≈4×/s at 60fps).
	if (--g.pollSkip <= 0) {
		g.pollSkip = 15;
		struct stat st {};
		if (::stat(g.path.c_str(), &st) == 0) {
			if (st.st_mtime != g.mtime) loadScript(st.st_mtime);
		} else if (g.mtime != 0) {
			std::fprintf(stderr, "[SCRIPT] %s removed — values restored\n",
			             g.path.c_str());
			unloadScript();
		}
	}

	for (const Track &t : g.tracks) {
		// shadowed = CLI/env at load; paramIsCliSet re-checked per tick so
		// a tune-server write takes a live track over immediately.
		if (t.shadowed || paramIsCliSet(t.ref)) continue;
		writeParam(t.ref, evalTrack(t, timer));
	}
}

} // namespace fds
