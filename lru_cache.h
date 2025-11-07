#pragma once
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

class LRUCache {
public:
    LRUCache(size_t capacity=1000): capacity(capacity) {}
    bool get(const std::string &key, std::string &value) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = map.find(key);
        if (it==map.end()) return false;
        nodes.splice(nodes.begin(), nodes, it->second);
        value = it->second->second;
        return true;
    }
    void put(const std::string &key, const std::string &value) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = map.find(key);
        if (it != map.end()) {
            it->second->second = value;
            nodes.splice(nodes.begin(), nodes, it->second);
            return;
        }
        if (nodes.size() >= capacity) {
            auto last = nodes.back();
            map.erase(last.first);
            nodes.pop_back();
        }
        nodes.emplace_front(key, value);
        map[key] = nodes.begin();
    }
    void erase(const std::string &key) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = map.find(key);
        if (it==map.end()) return;
        nodes.erase(it->second);
        map.erase(it);
    }
private:
    size_t capacity;
    std::list<std::pair<std::string,std::string>> nodes;
    std::unordered_map<std::string, decltype(nodes.begin())> map;
    std::mutex mtx;
};
