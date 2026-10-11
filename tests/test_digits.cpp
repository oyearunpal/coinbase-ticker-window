#include "doctest.h"
#include "../src/digits.hpp"

#include <cstdio>

using namespace ticker;

TEST_CASE("digits::put2: every value 0-99, exhaustively, against snprintf") {
    for (int v = 0; v <= 99; ++v) {
        char buf[3] = {};
        digits::put2(buf, v);
        char expected[3];
        std::snprintf(expected, sizeof(expected), "%02d", v);
        CHECK(buf[0] == expected[0]);
        CHECK(buf[1] == expected[1]);
    }
}

TEST_CASE("digits::put3: every value 0-999, exhaustively, against snprintf") {
    for (int v = 0; v <= 999; ++v) {
        char buf[4] = {};
        digits::put3(buf, v);
        char expected[4];
        std::snprintf(expected, sizeof(expected), "%03d", v);
        CHECK(buf[0] == expected[0]);
        CHECK(buf[1] == expected[1]);
        CHECK(buf[2] == expected[2]);
    }
}

TEST_CASE("digits::put2/put3: don't write past their declared width") {
    // Write into the middle of a larger buffer with sentinel bytes on
    // both sides; put2/put3 must touch only their own 2/3 bytes.
    char buf[7];
    for (char& c : buf) c = 'x';
    digits::put2(buf + 2, 42);
    CHECK(buf[0] == 'x');
    CHECK(buf[1] == 'x');
    CHECK(buf[2] == '4');
    CHECK(buf[3] == '2');
    CHECK(buf[4] == 'x');

    for (char& c : buf) c = 'x';
    digits::put3(buf + 2, 7);
    CHECK(buf[0] == 'x');
    CHECK(buf[1] == 'x');
    CHECK(buf[2] == '0');
    CHECK(buf[3] == '0');
    CHECK(buf[4] == '7');
    CHECK(buf[5] == 'x');
}
