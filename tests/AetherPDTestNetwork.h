#pragma once

#include "AetherPDTestTransport.h"
#include <iostream>

namespace {
using AetherPDTest::Network;
using AetherPDTest::Reply;
bool check(bool value, const char* message)
{
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return value;
}
} // namespace

