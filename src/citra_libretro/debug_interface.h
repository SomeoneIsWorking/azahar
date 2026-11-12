// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

namespace LibRetro::Debug {

/// Offers the frontend the debug interface (azahar_debug.h).
void PublishToFrontend();

/// Drops every watch; called when the game unloads.
void Reset();

} // namespace LibRetro::Debug
