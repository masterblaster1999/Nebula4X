#pragma once

// Minimal shared header for the test runner.
//
// Individual tests in this repo use their own local N4X_ASSERT macro.
// This file exists primarily so test_main.cpp can include a stable header
// without depending on any specific test framework.

#include <cstdint>
#include <string>

