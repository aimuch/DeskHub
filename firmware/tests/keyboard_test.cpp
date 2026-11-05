// Failure cases first: non-ASCII/control input; overrun; ambiguous 64-byte PSK;
// mode switches changing input; reveal surviving cancellation; deleting empty input;
// secrets left behind on exit. This model is shared by the real LVGL keyboard.
#include "ui_keyboard.hpp"
#include <cassert>
#include <iostream>
#include <set>
using dh::ui::Keyboard;
int main() {
    Keyboard k;
    assert(k.password() && !k.valid() && k.display().empty());
    for (char c : std::string("Test123!")) assert(k.append(c));
    assert(k.valid() && k.display() == "********");
    k.reveal(true); assert(k.display() == "Test123!");
    k.reveal(false); assert(k.display() == "********");
    assert(!k.append('\n') && !k.append('\0') && !k.append(static_cast<char>(0xff)));
    k.clear(); k.reveal(true); assert(k.display().empty());
    for (int i = 0; i < 63; ++i) assert(k.append('x'));
    assert(k.valid() && !k.append('x') && k.size() == 63);
    k.clear(); for (int i = 0; i < 64; ++i) assert(k.append('a'));
    assert(k.valid() && !k.append('a'));
    k.erase(); assert(k.size() == 63 && k.valid());
    k.clear(); k.erase(); assert(k.size() == 0 && !k.valid());
    k.reset(false); assert(!k.password());
    for (int i = 0; i < 32; ++i) assert(k.append(' '));
    assert(k.valid() && !k.append('a') && k.display() == std::string(32, ' '));
    k.reset(true); assert(k.size() == 0 && !k.valid());
    std::set<char> coverage;
    for (int page = 0; page < 4; ++page)
        for (const auto &row : Keyboard::rows(page)) for (char c : row) coverage.insert(c);
    coverage.insert(' ');
    for (char c = 32; c < 127; ++c) assert(coverage.count(c));
    // What the line under the field says: how many are missing, the limit, and when the field is full.
    k.reset(true); assert(k.missing() == 8 && k.limit() == 63 && !k.full());
    for (int i = 0; i < 5; ++i) k.append('x');
    assert(k.missing() == 3);
    for (int i = 0; i < 3; ++i) k.append('x');
    assert(k.missing() == 0 && !k.full() && k.limit() == 63);
    while (k.append('x')) {}
    assert(k.size() == 63 && k.full() && k.limit() == 63);   // not hex: 63 is the end
    k.reset(true); for (int i = 0; i < 63; ++i) k.append('a');
    assert(k.size() == 63 && !k.full() && k.limit() == 63);  // hex: one more is allowed
    k.append('a'); assert(k.size() == 64 && k.full() && k.limit() == 64);
    k.reset(false); assert(k.missing() == 0 && k.limit() == 32 && !k.full());
    for (int i = 0; i < 32; ++i) k.append('n');
    assert(k.full());
    std::cout << "PASS keyboard: bounds, ASCII, PSK, reveal, clear, all printable keys\n";
}
