// Replicated history branches: several copies of one model (each with its own parameter values, boundary values and
// history) side by side in ONE Abaqus job and restart chain. Copies never share a node or element, so each copy's
// residual, state update and output are exactly those of the copy run alone (verified bitwise on Abaqus 2024,
// docs/PERFORMANCE.md). Copy 0 keeps the original names and labels; copy k shifts node/element labels by k * offset
// and suffixes every set, surface, material, section-control, orientation and amplitude name.
//
// Scope: flat decks built from an explicit keyword list (nodes, elements, sets, surfaces, sections, materials incl.
// user materials, orientations, amplitudes, transforms, point masses, model-data boundary conditions). Anything else
// (parts/instances, constraints, contact, user elements, ...) is refused with the keyword named; the caller then
// runs branches as separate jobs.
#pragma once

#include "abaqus_files.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace abss::abq {

struct ReplicaNumbering {
    std::int64_t nodeOffset = 0;  ///< copy k: node label + k * nodeOffset (all original labels < nodeOffset)
    std::int64_t elemOffset = 0;
};

/// Offsets for a model whose largest labels are maxNode / maxElem (rounded up to a power of ten).
ReplicaNumbering replicaNumbering(std::int64_t maxNode, std::int64_t maxElem);

/// Name of an entity in copy k (k = 0: unchanged).
std::string replicaName(const std::string& name, int copy);

/// Model data (text before the first *STEP, includes already expanded) of copy k. Throws std::runtime_error naming the
/// first keyword that replication does not support. Copy 0 returns the text unchanged.
std::string replicateModelData(const std::string& modelData, int copy, const ReplicaNumbering& n);

/// One copy's step text (keywords as the history branches write them) with node/element/set/surface references of
/// copy k. Copy 0 returns the text unchanged.
std::string replicateStepData(const std::string& stepText, int copy, const ReplicaNumbering& n);

/// Merge the step texts of all copies of one step: procedure, controls and output requests from copy 0; boundary
/// conditions and loads of every copy in one block per keyword line. Throws if the copies' keyword sequences differ.
std::string mergeCopySteps(const std::vector<std::string>& perCopy);

/// Results of copy k in an increment of a replicated job, with copy-0 labels.
FilIncrement extractCopy(const FilIncrement& all, int copy, const ReplicaNumbering& n);

}  // namespace abss::abq
