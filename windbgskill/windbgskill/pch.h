// pch.h - Precompiled header
// Put stable, rarely-changed headers here for faster incremental builds.

#ifndef PCH_H
#define PCH_H

#include "framework.h"

// WinDbg debug engine SDK
#include <dbgeng.h>
#pragma comment(lib, "dbgeng.lib")

// Standard library headers used across the project
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>
#include <cstdarg>

#endif // PCH_H
