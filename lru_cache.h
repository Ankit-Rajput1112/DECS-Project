#include <atomic>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

class LRUCache {
public:
    explicit LRUCache(size_t capacity = 1000) : capacity_(capacity), hit_count_(0), miss_count_(0) {}

    // Non-copyable
    LRUCache(const LRUCache&) = delete;
    LRUCache& operator=(const LRUCache&) = delete;

    bool get(const std::string &key, std::string &value) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = map_.find(key);
        if (it == map_.end()) {
            miss_count_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        // Move node to front (most-recently-used)
        nodes_.splice(nodes_.begin(), nodes_, it->second);
        value = it->second->second;
        hit_count_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Put or update
    void put(const std::string &key, const std::string &value) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            // update existing node and move to front
            it->second->second = value;
            nodes_.splice(nodes_.begin(), nodes_, it->second);
            return;
        }

        // Evict last node if needed
        if (nodes_.size() >= capacity_) {
            auto last = nodes_.back();
            map_.erase(last.first);
            nodes_.pop_back();
        }

        nodes_.emplace_front(key, value);
        map_[key] = nodes_.begin();
    }

    // Erase key if present
    void erase(const std::string &key) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = map_.find(key);
        if (it == map_.end()) return;
        nodes_.erase(it->second);
        map_.erase(it);
    }

    // Clear all entries
    void clear() {
        std::lock_guard<std::mutex> lk(mtx_);
        nodes_.clear();
        map_.clear();
        hit_count_.store(0, std::memory_order_relaxed);
        miss_count_.store(0, std::memory_order_relaxed);
    }

    // Current #elements
    size_t size() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return nodes_.size();
    }
    
    size_t capacity() const noexcept {
        return capacity_;
    }

    // hit/miss counters
    uint64_t hits() const noexcept { return hit_count_.load(std::memory_order_relaxed); }
    uint64_t misses() const noexcept { return miss_count_.load(std::memory_order_relaxed); }

private:
    size_t capacity_;
    // list of (key, value)
    std::list<std::pair<std::string, std::string>> nodes_;
    std::unordered_map<std::string, decltype(nodes_.begin())> map_;
    mutable std::mutex mtx_;
    
    std::atomic<uint64_t> hit_count_;
    std::atomic<uint64_t> miss_count_;
};
