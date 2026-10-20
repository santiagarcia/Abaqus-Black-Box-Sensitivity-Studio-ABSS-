// Readers for Abaqus/Standard text outputs used by the oracle (verified on Abaqus 2024, scout_abaqus2024.md):
//  * ASCII results file (*FILE FORMAT, ASCII): double precision (16 significant digits) U, RF, CF, S, E/LE, PE, SDV.
//  * *ELEMENT MATRIX OUTPUT, OUTPUT FILE=USER DEFINED: per-element stiffness (lower triangle, node-major DOF order).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace abss::abq {

struct FilElementPoint {
    std::int64_t element = 0;
    int ip = 0;
    int sectionPoint = 0;
    std::map<std::string, std::vector<double>> values;  ///< "S", "E", "LE", "PE", "SDV"
};

struct FilIncrement {
    int step = 0;
    int increment = 0;
    double totalTime = 0;
    double stepTime = 0;
    std::map<std::int64_t, std::vector<double>> U, RF, CF;  ///< node label -> components
    std::map<std::int64_t, std::vector<double>> V, A;       ///< velocities, accelerations (dynamic steps)
    std::vector<FilElementPoint> points;                    ///< in file order
};

/// Parse an ASCII .fil file. Throws std::runtime_error on malformed records.
std::vector<FilIncrement> readFil(const std::string& path);

/// Model definition written at the start of a .fil file (records 1900 element, 1901 node). For a part/instance
/// model the numbers are Abaqus's global internal numbers (not the part labels).
struct FilModel {
    std::map<std::int64_t, std::vector<double>> nodes;                                         ///< number -> coordinates
    std::map<std::int64_t, std::pair<std::string, std::vector<std::int64_t>>> elements;        ///< number -> (type, nodes)
};
FilModel readFilModel(const std::string& path);

struct EmoBlock {
    std::int64_t element = 0;
    std::vector<std::int64_t> nodes;
    std::vector<int> dofs;            ///< active DOFs per node
    std::vector<double> K;            ///< dense symmetric n x n, n = nodes*dofs, row-major
};

/// Parse an element-matrix-output file; returns blocks for every (step, increment).
std::map<std::pair<int, int>, std::vector<EmoBlock>> readEmo(const std::string& path);

}  // namespace abss::abq
