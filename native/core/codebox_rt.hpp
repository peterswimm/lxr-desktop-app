// Runtime for C++ translated from RNBO codebox code (native/tools/codebox2cpp.py):
// the codebox value types (number, list, fixed array, liststore) with codebox
// semantics. Every number is a double; indices are doubles truncated to integers.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#if defined(__FP_FAST_FMA) && !defined(LXR_ALLOW_FP_CONTRACT)
// The translated code emulates float32 rounding with double arithmetic; a fused
// multiply-add changes those results. Build with -ffp-contract=off (and without
// -ffast-math); see native/CMakeLists.txt.
#endif

namespace cbx {

inline std::size_t index(double i) { return static_cast<std::size_t>(static_cast<long long>(i)); }

inline double min(double a, double b) { return a < b ? a : b; }
inline double max(double a, double b) { return a > b ? a : b; }
inline double clamp(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline double isnan(double x) { return std::isnan(x) ? 1.0 : 0.0; }
inline double round(double x) { return std::floor(x + 0.5); }

// a codebox list: a growable array of numbers, copied by value
struct List {
	std::vector<double> v;

	List() = default;
	List(std::initializer_list<double> l) : v(l) {}

	double& operator[](double i) {
		std::size_t k = index(i);
		if (k >= v.size()) v.resize(k + 1, 0.0);
		return v[k];
	}
	double operator[](double i) const {
		std::size_t k = index(i);
		return k < v.size() ? v[k] : 0.0;
	}
	double length() const { return static_cast<double>(v.size()); }
	void push(double x) { v.push_back(x); }
};

// FixedDoubleArray. Out of range it behaves like the JavaScript typed arrays the port
// is tested with (rnbo/test/codebox.mjs): a write is dropped and a read gives NaN.
// The LXR code keeps its indices in range; this only keeps a mistake from crashing.
template <int N>
struct Arr {
	double v[N] = {};
	double outside = 0.0;
	double& operator[](double i) {
		std::size_t k = index(i);
		if (k < static_cast<std::size_t>(N)) return v[k];
		outside = std::nan("");
		return outside;
	}
	double operator[](double i) const {
		std::size_t k = index(i);
		return k < static_cast<std::size_t>(N) ? v[k] : std::nan("");
	}
	static constexpr int size() { return N; }
};

// liststore: numbered slots holding lists (RNBO saves them in presets)
struct ListStore {
	std::vector<std::vector<double>> slots;
	std::size_t maxSize;

	ListStore(int slotCount, int maxListSize) : slots(static_cast<std::size_t>(slotCount)), maxSize(static_cast<std::size_t>(maxListSize)) {}

	void store(double i, const List& l) {
		std::size_t k = index(i);
		if (k >= slots.size()) return;
		slots[k] = l.v;
		if (slots[k].size() > maxSize) slots[k].resize(maxSize);
	}
	List lookup(double i) const {
		List l;
		std::size_t k = index(i);
		if (k < slots.size()) l.v = slots[k];
		return l;
	}
	void clear() {
		for (auto& s : slots) s.clear();
	}
};

struct ParamInfo {
	const char* name;
	double min, max, def;
};

} // namespace cbx
