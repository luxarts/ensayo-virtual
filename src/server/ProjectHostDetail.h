#pragma once
// Internals of ProjectHost exposed for unit tests only.
#include "core/Project.h"
#include <functional>
#include <optional>
namespace ensayo::hostdetail
{
// The host user's id may only be claimed from the host machine itself (it is visible in every project.json).
bool mayClaimUser (const Id& user, const Id& hostUser, bool fromLoopback) noexcept;

// Writes the whole buffer through `sendSome` (returns bytes accepted > 0, 0 when it would block, < 0 on error),
// sleeping briefly while it would block. Fails when no byte is accepted for `stallMs` in a row (the deadline is
// renewed on every progress, so a slow but steady peer is never dropped) or when `shouldAbort` returns true.
bool writeWithStallTimeout (const std::function<int (const char*, size_t)>& sendSome, const char* data, size_t len,
                            int64_t stallMs, const std::function<bool()>& shouldAbort = {});

// An integral number in [min, max] (int, int64 or an integral double within ±2^53); nullopt otherwise.
std::optional<int64_t> readInteger (const juce::var&, int64_t min, int64_t max);
}
