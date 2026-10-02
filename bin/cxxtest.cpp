// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Rigby Foundation
// cxxtest: the C++ runtime on sic (libc++, libc++abi, libunwind): exceptions
// across frames and threads, RTTI, containers, strings, streams and
// std::format, threads with mutexes and condition variables, thread_local,
// global constructors, filesystem, chrono, smart pointers.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <unordered_map>
#include <variant>
#include <vector>

static int passed, failed;
static void check(bool ok, const char *what)
{
    if (ok) passed++;
    else { failed++; std::cout << "cxxtest: FAILED: " << what << "\n"; }
}

// a global constructor must have run before main
static struct Early { int v; Early() : v(42) {} } early;

struct Shape { virtual ~Shape() = default; virtual int sides() const = 0; };
struct Tri : Shape { int sides() const override { return 3; } };
struct Quad : Shape { int sides() const override { return 4; } };

struct MyError : std::runtime_error { int code; MyError(int c) : std::runtime_error("my error"), code(c) {} };
static void deep(int n) { if (n == 0) throw MyError(7); deep(n - 1); }

thread_local int tls_counter = 0;

int main()
{
    check(early.v == 42, "global constructor");

    // exceptions: through several frames, by base class, rethrown, nested
    try { deep(10); check(false, "throw through frames"); }
    catch (const MyError &e) { check(e.code == 7 && std::string(e.what()) == "my error", "catch a derived exception"); }
    try { try { throw std::out_of_range("inner"); } catch (...) { throw; } }
    catch (const std::logic_error &e) { check(std::string(e.what()) == "inner", "rethrow, caught by base"); }
    try { std::vector<int> v(3); (void)v.at(10); check(false, "at() throws"); }
    catch (const std::out_of_range &) { check(true, "at() throws out_of_range"); }
    bool dtor_ran = false;
    try { struct Guard { bool *f; ~Guard() { *f = true; } } g{&dtor_ran}; throw 5; }
    catch (int x) { check(x == 5 && dtor_ran, "stack unwinding runs destructors"); }

    // RTTI
    std::unique_ptr<Shape> s = std::make_unique<Quad>();
    check(dynamic_cast<Quad *>(s.get()) && !dynamic_cast<Tri *>(s.get()) && typeid(*s) == typeid(Quad), "dynamic_cast, typeid");

    // containers and algorithms
    std::vector<int> v{5, 3, 9, 1, 7};
    std::sort(v.begin(), v.end());
    check((v == std::vector<int>{1, 3, 5, 7, 9}), "sort");
    std::map<std::string, int> m{{"b", 2}, {"a", 1}};
    std::unordered_map<int, std::string> um;
    for (int i = 0; i < 1000; i++) um[i] = std::to_string(i * i);
    check(m.begin()->first == "a" && um[31] == "961" && um.size() == 1000, "map, unordered_map, to_string");

    // strings, streams, format, parsing
    std::ostringstream os;
    os << "pi=" << 3.14159 << " n=" << 42;
    check(os.str() == "pi=3.14159 n=42", "ostringstream");
    check(std::format("{:>5}|{:.2f}|{:x}", 7, 2.5, 255) == "    7|2.50|ff", "std::format");
    check(std::stoi("123") + std::stod("0.5") == 123.5, "stoi, stod");

    // optional, variant, function
    std::optional<int> o = 3;
    std::variant<int, std::string> var = std::string("x");
    std::function<int(int)> twice = [](int x) { return 2 * x; };
    check(o.value_or(0) == 3 && std::holds_alternative<std::string>(var) && twice(21) == 42, "optional, variant, function");

    // threads: a mutex-protected counter, a condition variable, thread_local, an exception in a thread
    std::mutex mu;
    std::condition_variable cv;
    int total = 0, ready = 0;
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; t++)
        ts.emplace_back([&] {
            for (int i = 0; i < 1000; i++) { tls_counter++; std::lock_guard<std::mutex> l(mu); total++; }
            std::lock_guard<std::mutex> l(mu);
            ready += tls_counter == 1000;
            cv.notify_one();
        });
    {
        std::unique_lock<std::mutex> l(mu);
        cv.wait(l, [&] { return ready == 4; });
    }
    for (auto &t : ts) t.join();
    check(total == 4000 && ready == 4 && tls_counter == 0, "threads, mutex, condition_variable, thread_local");
    std::string caught;
    std::thread([&] { try { throw std::runtime_error("in a thread"); } catch (const std::exception &e) { caught = e.what(); } }).join();
    check(caught == "in a thread", "exception inside a thread");

    // shared_ptr across threads
    auto sp = std::make_shared<int>(9);
    std::thread([sp] { (void)*sp; }).join();
    check(sp.use_count() == 1 && *sp == 9, "shared_ptr");

    // filesystem and chrono
    auto t0 = std::chrono::steady_clock::now();
    int files = 0;
    for (auto &e : std::filesystem::directory_iterator("/bin")) { (void)e; files++; }
    check(files > 10 && std::filesystem::exists("/bin/cxxtest"), "filesystem directory_iterator");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(20), "chrono, sleep_for");

    std::cout << "cxxtest: " << passed << " passed, " << failed << " failed" << std::endl;
    return failed ? 1 : 0;
}
