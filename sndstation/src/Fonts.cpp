#include "Fonts.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <sndstation/style.h>

#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace lxrapp {

namespace {

constexpr const char* kRegular = "OpenSans-Regular.ttf";
constexpr const char* kBold = "OpenSans-Bold.ttf";

std::vector<fs::path> candidates() {
	const char* env = SDL_getenv("LXR_FONT_DIR");
	if (env && *env) return {fs::absolute(env)};
	std::vector<fs::path> dirs;
	if (char* base = SDL_GetBasePath()) {
		const fs::path bin(base);
		SDL_free(base);
		dirs.push_back(bin / ".." / "share" / "lxr-sndstation" / "fonts");
		dirs.push_back(bin / "fonts");
	}
#ifdef LXR_SOURCE_FONT_DIR
	dirs.emplace_back(LXR_SOURCE_FONT_DIR);
#endif
	return dirs;
}

// both files are there and SDL_ttf opens them, as the SDK will
bool opens(const fs::path& dir, std::string* why) {
	std::error_code ec;
	for (const char* f : {kRegular, kBold}) {
		if (!fs::is_regular_file(dir / f, ec)) {
			*why = (dir / f).string() + " is not there";
			return false;
		}
	}
	if (TTF_Init() != 0) {
		*why = std::string("SDL_ttf: ") + TTF_GetError();
		return false;
	}
	bool ok = true;
	for (const char* f : {kRegular, kBold}) {
		TTF_Font* font = TTF_OpenFont((dir / f).string().c_str(), 12);
		if (!font) {
			*why = (dir / f).string() + " will not open: " + TTF_GetError();
			ok = false;
			break;
		}
		TTF_CloseFont(font);
	}
	TTF_Quit();
	return ok;
}

} // namespace

bool useOpenSans(std::string* dir, std::string* why) {
	std::string tried;
	for (const fs::path& c : candidates()) {
		std::error_code ec;
		fs::path d = fs::weakly_canonical(c, ec);
		if (ec) d = c.lexically_normal();
		std::string problem;
		if (!opens(d, &problem)) {
			tried += "\n  " + problem;
			continue;
		}
		sndstation::Style s = sndstation::style();
		s.font.sans = (d / kRegular).string();
		s.font.bold = (d / kBold).string();
		s.font.mono = s.font.sans;   // values in the same face
		sndstation::setStyle(s);
		if (dir) *dir = d.string();
		return true;
	}
	if (why) *why = "no Open Sans (" + std::string(kRegular) + ", " + kBold + "); the app draws in nothing else:" + tried;
	return false;
}

} // namespace lxrapp
