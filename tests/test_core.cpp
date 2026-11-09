#include "../src/core.hpp"
#include <cassert>
#include <iostream>
int main() {
    using namespace wavecue;
    Cues c;
    assert(add_mark(c, 2000, 30000));
    assert(add_mark(c, 1000, 30000));
    assert(!add_mark(c, 1004, 30000));
    assert((c.marks == std::vector<int64_t>{1000, 2000}));
    assert(set_start(c, 1500, 30000));
    assert(set_end(c, 25000, 30000));
    assert(!set_start(c, 26000, 30000));
    assert(!set_end(c, 1000, 30000));
    assert(valid_range(c, 30000));
    assert(clamp_ms(40000, 30000) == 30000);
    assert(path_hash(L"C:\\Music\\TRACK.mp3") == path_hash(L"c:\\music\\track.mp3"));
    std::cout << "Tests du noyau reussis.\n";
}
