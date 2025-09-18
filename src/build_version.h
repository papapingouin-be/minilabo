#pragma once

#include <cstdint>

namespace BuildInfo {
// Update these constants to reflect the current firmware semantic version.
// Use MAJOR.MINOR.PATCH for released builds and optionally append
// pre-release/build metadata tags (see RFC 9110 / Semantic Versioning 2.0.0).
constexpr std::uint8_t kFirmwareMajor = 1;
constexpr std::uint8_t kFirmwareMinor = 0;
constexpr std::uint32_t kFirmwarePatch = 5;

// Optional identifiers. Leave empty when not required.
constexpr const char *kFirmwarePreReleaseTag = "";
constexpr const char *kFirmwareBuildMetadata = "";

inline bool HasPreReleaseTag() {
  return kFirmwarePreReleaseTag != nullptr && kFirmwarePreReleaseTag[0] != '\0';
}

inline bool HasBuildMetadata() {
  return kFirmwareBuildMetadata != nullptr && kFirmwareBuildMetadata[0] != '\0';
}
}  // namespace BuildInfo

