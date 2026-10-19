// ABSS project persistence: a results file that reopens without Abaqus.
//
// Format (".abss"): a single file = 8-byte magic "ABSSRES1", uint64 manifest length, UTF-8 JSON manifest
// (metadata, mesh, frames, fields, parameters, quality evidence, series index), then raw little-endian
// float64 payload referenced by byte offsets from the manifest.
#pragma once

#include "abss/core/results.hpp"

#include <string>

namespace abss {

struct ProjectMeta {
    std::string appVersion = "0.1.0";
    std::string createdUtc;
    std::string inpPath;
    std::string inpSha256;           ///< hash of the root INP at run time (empty for demo)
    std::vector<std::string> dependencies;
    std::string solverBackend;
    std::string computeBackend;      ///< e.g. "Kokkos OpenMP (8 threads)"
    std::string settingsJson;        ///< run configuration (order, steps, scheme) as JSON text
};

void saveProject(const std::string& path, const ResultStore& store, const ProjectMeta& meta);
ResultStore loadProject(const std::string& path, ProjectMeta* meta = nullptr);

/// CSV of one location's history: frame, step, increment, step time, total time, then one column per series.
std::string historyCsv(const ResultStore& store, int field, std::int64_t location, int component,
                       const std::vector<DerivativeSpec>& series);

/// SHA-256 of a file (hex). Empty string if the file cannot be read.
std::string sha256File(const std::string& path);

}  // namespace abss
