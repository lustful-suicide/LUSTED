#pragma once
// Process startup helpers: script discovery, first-run fetch, CLI parsing.
// Small interface over filesystem/network details so main() stays orchestration.
#include <filesystem>
#include <string>
#include <vector>

// Sorted C:/LUSTED/Luas/*.{luau,lua} plus Luas/official/*.
std::vector<std::filesystem::path> CollectLuas(const std::filesystem::path& dir);

// First run: fetch C:/LUSTED/Luas/official/ from GitHub (raw). Override with
// LUSTED_OFFICIAL_LUAS_URL ("<base>/", trailing slash optional).
void EnsureOfficialLuas();

// True for "version-<hash>" and bare hex hashes.
bool LooksLikeHash(const std::string& s);
