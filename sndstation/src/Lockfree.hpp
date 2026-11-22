// The two lock-free pieces between the threads: a single-producer single-consumer ring
// (UI -> audio, MIDI -> audio) and a triple buffer (audio -> UI).
#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace lxrapp {

// One producer thread, one consumer thread, no locks and no allocation. push() fails
// when full rather than wait.
template <class T, size_t N>
class Ring {
	static_assert((N & (N - 1)) == 0, "N must be a power of two");

public:
	bool push(const T& x) {
		const size_t w = w_.load(std::memory_order_relaxed);
		if (w - r_.load(std::memory_order_acquire) == N) return false;
		buf_[w & (N - 1)] = x;
		w_.store(w + 1, std::memory_order_release);
		return true;
	}
	bool pop(T& x) {
		const size_t r = r_.load(std::memory_order_relaxed);
		if (r == w_.load(std::memory_order_acquire)) return false;
		x = buf_[r & (N - 1)];
		r_.store(r + 1, std::memory_order_release);
		return true;
	}

private:
	std::array<T, N> buf_{};
	alignas(64) std::atomic<size_t> w_{0};
	alignas(64) std::atomic<size_t> r_{0};
};

// The writer always has a slot of its own to fill (back()), and publish() hands it over;
// the reader takes the newest one published (read()) and keeps it until the next.
// Neither side ever waits for the other.
template <class T>
class TripleBuffer {
public:
	T& back() { return slot_[back_]; }
	void publish() {
		back_ = middle_.exchange(back_ | kFresh, std::memory_order_acq_rel) & kIndex;
	}
	// the newest published value; false when nothing new was published since the last call
	bool read() {
		if (!(middle_.load(std::memory_order_relaxed) & kFresh)) return false;
		front_ = middle_.exchange(front_, std::memory_order_acq_rel) & kIndex;
		return true;
	}
	const T& front() const { return slot_[front_]; }

private:
	static constexpr int kFresh = 4, kIndex = 3;
	T slot_[3]{};
	int back_ = 0, front_ = 1;
	std::atomic<int> middle_{2};
};

} // namespace lxrapp
