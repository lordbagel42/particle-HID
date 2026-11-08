#pragma once

#include <cstddef>

namespace phid {

// Fixed-capacity FIFO. Not thread-safe; callers hold the library lock.
template <typename T, size_t N>
class Ring {
public:
    static constexpr size_t capacity = N;

    size_t size() const {
        return count_;
    }
    bool empty() const {
        return count_ == 0;
    }
    bool full() const {
        return count_ == N;
    }
    bool push(const T& v) {
        if (full()) {
            return false;
        }
        items_[(head_ + count_) % N] = v;
        ++count_;
        return true;
    }
    T& front() {
        return items_[head_];
    }
    const T& front() const {
        return items_[head_];
    }
    T& back() {
        return items_[(head_ + count_ - 1) % N];
    }
    void pop() {
        if (count_) {
            head_ = (head_ + 1) % N;
            --count_;
        }
    }
    void clear() {
        head_ = 0;
        count_ = 0;
    }

private:
    T items_[N] = {};
    size_t head_ = 0;
    size_t count_ = 0;
};

} // namespace phid
