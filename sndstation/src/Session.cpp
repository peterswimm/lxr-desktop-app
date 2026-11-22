#include "Session.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <unistd.h>

namespace lxrapp {

void applyAppSettings(lxr::LxrInstrument& in) {
	in.setParam(lxr::kIdSeqMode, 1);
	in.setParam(lxr::kIdPlay, 0);
	in.setParam(lxr::kIdOutputs, 0);
	in.dropChanges();
}

void demoPattern(lxr::LxrInstrument& in) {
	lxr::LxrSeq& s = in.seq();
	for (int st = 0; st < 16; st += 4) s.setMainStep(0, 0, st, 1);
	s.setMainStep(0, 3, 4, 1);
	s.setMainStep(0, 3, 12, 1);
	for (int st = 0; st < 16; st += 2) {
		if (st != 14) s.setMainStep(0, 5, st, 1);
		if (st % 4 == 2) s.setStepField(0, 5, st, 0, 70);
	}
	s.setMainStep(0, 6, 14, 1);
}

std::unique_ptr<lxr::LxrInstrument> freshInstrument() {
	auto in = std::make_unique<lxr::LxrInstrument>();
	demoPattern(*in);
	applyAppSettings(*in);
	return in;
}

bool loadSession(const std::string& path, lxr::LxrInstrument& in, std::string* why) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		if (why) *why = "no session yet";
		return false;
	}
	std::stringstream ss;
	ss << f.rdbuf();
	if (!in.loadState(ss.str())) {
		if (why) *why = "not an LXR session";
		return false;
	}
	applyAppSettings(in);
	return true;
}

bool writeSession(const std::string& path, const std::string& text, std::string* why) {
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path p(path);
	if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
	const std::string tmp = path + ".tmp";
	FILE* f = std::fopen(tmp.c_str(), "wb");
	if (!f) {
		if (why) *why = std::strerror(errno);
		return false;
	}
	const bool wrote = std::fwrite(text.data(), 1, text.size(), f) == text.size();
	// to the card itself before the rename: a handheld can lose power at any moment
	const bool flushed = std::fflush(f) == 0 && fsync(fileno(f)) == 0;
	const bool closed = std::fclose(f) == 0;
	if (!wrote || !flushed || !closed) {
		if (why) *why = "short write";
		std::remove(tmp.c_str());
		return false;
	}
	fs::rename(tmp, p, ec);
	if (ec) {
		if (why) *why = ec.message();
		std::remove(tmp.c_str());
		return false;
	}
	return true;
}

} // namespace lxrapp
