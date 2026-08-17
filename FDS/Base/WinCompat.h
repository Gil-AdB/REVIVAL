#pragma once
// Windows shims for the handful of POSIX facilities this tree uses.
//
// The whole file is inside `#ifdef _WIN32`, and every call site includes it
// behind that same guard, so on macOS / Linux / emscripten this header
// contributes exactly nothing — the preprocessed translation units there are
// byte-for-byte what they were before the Windows port.
//
// It covers both Windows toolchains, which differ from each other as much as
// either differs from POSIX:
//   * MinGW-w64 ships <unistd.h> and <sys/types.h> but neither <dirent.h>
//     entry points we need with POSIX names, nor <libgen.h>, nor realpath().
//   * MSVC ships none of those headers at all and spells the CRT entry
//     points with a leading underscore (_mkdir, _access, _isatty, ...).
//
// The macros below are FUNCTION-LIKE on purpose: they fire only on a call,
// never on a bare identifier, so they cannot collide with a member, a local
// or a type of the same name elsewhere in a large translation unit.

#ifdef _WIN32

#include <direct.h>    // _mkdir, _chdir, _getcwd
#include <io.h>        // _access, _isatty
#include <stdio.h>     // _fileno
#include <stdlib.h>    // _fullpath, _putenv_s, _MAX_PATH
#include <string.h>
#include <string>

// wingdi.h #defines TRANSPARENT and OPAQUE (background-mode constants) —
// which are ALSO the enumerator names of barry::TBlendMode, so every
// `TheOtherBarry<barry::TBlendMode::TRANSPARENT, ...>` instantiation becomes
// `...::1` and the compiler reports "parse error in template argument list".
// This is not hypothetical: it is what the MinGW cross-build actually hit in
// DEMO/Snapshot.cpp the moment this header started pulling in windows.h.
// NOGDI keeps wingdi.h out (nothing in this tree draws through GDI — the
// Win32 backends were deleted years ago); the #undefs below are the belt to
// that braces, for the case where something else included windows.h first.
#ifndef NOGDI
#define NOGDI
#endif
#include <windows.h>
#undef TRANSPARENT
#undef OPAQUE
// minwindef.h still #defines the 16-bit-era pointer qualifiers `far` and
// `near` to nothing. This tree uses `far` as an ordinary variable name (e.g.
// `TriMesh* far = appendTriMesh(...)` in DEMO/Snapshot.cpp's transparency
// test scene), which the macro silently deletes — the compiler then reports
// "expected unqualified-id before '=' token" on a line that looks perfectly
// fine. `IN`/`OUT`/`small` are the same class of hazard.
#undef far
#undef near
#undef IN
#undef OUT
#undef small

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef F_OK
#define F_OK 0
#endif

namespace fdswin {

// realpath(3): canonicalize. _fullpath is the CRT equivalent and, like
// realpath, returns null on failure. Unlike realpath it does NOT require the
// file to exist — every caller in this tree only uses the result as a cache
// key or a comparison key, so that difference is immaterial to them.
inline char *realpath_(const char *path, char *resolved) {
	return _fullpath(resolved, path, PATH_MAX);
}

// mkdir(2) with the mode argument dropped — Windows has no POSIX mode bits.
inline int mkdir_(const char *path, int /*mode*/) { return _mkdir(path); }

// setenv(3). _putenv_s always overwrites, so honour `overwrite` explicitly.
inline int setenv_(const char *name, const char *value, int overwrite) {
	if (!overwrite && getenv(name)) return 0;
	return _putenv_s(name, value);
}

} // namespace fdswin

#define realpath(p, r)       ::fdswin::realpath_((p), (r))
#define mkdir(p, m)          ::fdswin::mkdir_((p), (m))
#define setenv(n, v, o)      ::fdswin::setenv_((n), (v), (o))
#define access(p, m)         _access((p), (m))
#define isatty(fd)           _isatty(fd)
#define fileno(f)            _fileno(f)
#define chdir(p)             _chdir(p)
#define getcwd(b, n)         _getcwd((b), (int)(n))

// ---------------------------------------------------------------------------
// <dirent.h> — just enough of it. Only d_name is read anywhere in this tree
// (FDS/RENDER/EnvBake.cpp, DEMO/MaterialImport.cpp), so d_type and friends
// are deliberately absent rather than faked: a caller that starts using one
// should get a compile error here, not a silent wrong answer.
// ---------------------------------------------------------------------------
struct dirent {
	char d_name[MAX_PATH];
};

struct DIR {
	HANDLE           h = INVALID_HANDLE_VALUE;
	WIN32_FIND_DATAA fd{};
	bool             pending = false;   // fd holds an unconsumed entry
	::dirent         ent{};
};

inline DIR *opendir(const char *path) {
	std::string pat(path);
	if (!pat.empty() && pat.back() != '/' && pat.back() != '\\') pat += '\\';
	pat += '*';
	DIR *d = new DIR();
	d->h = ::FindFirstFileA(pat.c_str(), &d->fd);
	if (d->h == INVALID_HANDLE_VALUE) { delete d; return nullptr; }
	d->pending = true;
	return d;
}

inline struct dirent *readdir(DIR *d) {
	if (!d) return nullptr;
	if (!d->pending) {
		if (!::FindNextFileA(d->h, &d->fd)) return nullptr;
	}
	d->pending = false;
	strncpy(d->ent.d_name, d->fd.cFileName, MAX_PATH - 1);
	d->ent.d_name[MAX_PATH - 1] = '\0';
	return &d->ent;
}

inline int closedir(DIR *d) {
	if (!d) return -1;
	if (d->h != INVALID_HANDLE_VALUE) ::FindClose(d->h);
	delete d;
	return 0;
}

#endif // _WIN32
