#pragma once

#include "saturn/core/memory_image.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace test {

using Function = void (*)();

struct Case {
    std::string name;
    Function function;
};

class Registry {
public:
    void add(std::string name, Function function) { cases_.push_back({std::move(name), function}); }

    int run() const {
        int failures = 0;
        for (const auto& test : cases_) {
            try {
                test.function();
                std::cout << "[pass] " << test.name << '\n';
            } catch (const std::exception& error) {
                ++failures;
                std::cerr << "[fail] " << test.name << ": " << error.what() << '\n';
            }
        }
        std::cout << cases_.size() - failures << '/' << cases_.size() << " tests passed\n";
        return failures == 0 ? 0 : 1;
    }

private:
    std::vector<Case> cases_;
};

inline saturn::core::MemoryImage imageFromWords(
    saturn::core::Address base, std::initializer_list<std::uint16_t> words) {
    std::vector<std::byte> bytes;
    bytes.reserve(words.size() * 2);
    for (const auto word : words) {
        bytes.push_back(static_cast<std::byte>((word >> 8U) & 0xFFU));
        bytes.push_back(static_cast<std::byte>(word & 0xFFU));
    }
    return {base, std::move(bytes)};
}

} // namespace test

#define REQUIRE(condition) \
    do { \
        if (!(condition)) { \
            throw std::runtime_error(std::string("requirement failed: ") + #condition); \
        } \
    } while (false)
