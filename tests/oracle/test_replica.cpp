// Replicated history branches: deck text transforms (no Abaqus needed). The Abaqus-level equivalence of copies is
// verified separately (benchmarks/nasa_stri, docs/PERFORMANCE.md).
#include "../../src/oracle/abaqus_replica.hpp"

#include <doctest/doctest.h>

#include <stdexcept>

using namespace abss::abq;

namespace {
const char* kModel =
    "*HEADING\n"
    "two elements\n"
    "*NODE, NSET=ALLN\n"
    "1, 0., 0.\n2, 1., 0.\n3, 1., 1.\n4, 0., 1.\n"
    "*ELEMENT, TYPE=CPS4, ELSET=EL\n"
    "1, 1, 2, 3, 4\n"
    "*NSET, NSET=LEFT\n1, 4\n"
    "*NSET, NSET=GEN, GENERATE\n1, 4, 1\n"
    "*ELSET, ELSET=BOTH\nEL\n"
    "*SURFACE, NAME=TOPS\nEL, S3\n"
    "*SOLID SECTION, ELSET=EL, MATERIAL=STEEL\n1.\n"
    "*MATERIAL, NAME=STEEL\n*ELASTIC\n200000., 0.3\n*PLASTIC\n250., 0.\n"
    "*INITIAL CONDITIONS, TYPE=SOLUTION, USER\n"
    "*FILE FORMAT, ASCII\n";
}

TEST_CASE("Copy 0 is the model itself; copy k shifts labels and renames every entity") {
    const auto n = replicaNumbering(4, 1);
    CHECK(n.nodeOffset == 10);
    CHECK(n.elemOffset == 10);
    CHECK(replicateModelData(kModel, 0, n) == kModel);
    const auto c2 = replicateModelData(kModel, 2, n);
    CHECK(c2.find("*HEADING") == std::string::npos);            // written once, by copy 0
    CHECK(c2.find("*FILE FORMAT") == std::string::npos);
    CHECK(c2.find("TYPE=SOLUTION") == std::string::npos);       // user initialisation applies to every element
    CHECK(c2.find("*NODE, NSET=ALLN_ABSSC2\n21, 0., 0.\n22, 1., 0.") != std::string::npos);
    CHECK(c2.find("ELSET=EL_ABSSC2\n21, 21, 22, 23, 24\n") != std::string::npos);
    CHECK(c2.find("*NSET, NSET=LEFT_ABSSC2\n21, 24\n") != std::string::npos);
    CHECK(c2.find("GENERATE\n21, 24, 1\n") != std::string::npos);  // increment unchanged
    CHECK(c2.find("*ELSET, ELSET=BOTH_ABSSC2\nEL_ABSSC2\n") != std::string::npos);
    CHECK(c2.find("NAME=TOPS_ABSSC2\nEL_ABSSC2, S3\n") != std::string::npos);
    CHECK(c2.find("ELSET=EL_ABSSC2, MATERIAL=STEEL_ABSSC2") != std::string::npos);
    CHECK(c2.find("*MATERIAL, NAME=STEEL_ABSSC2\n*ELASTIC\n200000., 0.3\n*PLASTIC\n250., 0.\n") != std::string::npos);
}

TEST_CASE("Unsupported model keywords are refused by name, never silently copied") {
    const auto n = replicaNumbering(4, 1);
    CHECK_THROWS_WITH_AS(replicateModelData(std::string(kModel) + "*TIE, NAME=T\nA, B\n", 1, n),
                         doctest::Contains("*TIE"), std::runtime_error);
    CHECK_THROWS_AS(replicateModelData("*PART, NAME=P\n*END PART\n", 1, n), std::runtime_error);
}

TEST_CASE("Step texts of all copies merge into one step; results split back per copy") {
    const auto n = replicaNumbering(4, 1);
    const std::string st =
        "*STEP, NAME=ABSS_INC_2, NLGEOM=NO, INC=1\n*STATIC, DIRECT\n0.1, 0.1\n*BOUNDARY, OP=NEW\n1, 1, 1, 0.5\nLEFT, 2, 2, 0.\n"
        "*DSLOAD, OP=NEW\nTOPS, P, 3.\n*DLOAD, OP=NEW\n1, P2, 4.\n*NODE FILE, FREQUENCY=1\nU, RF\n*END STEP\n";
    const auto merged = mergeCopySteps({replicateStepData(st, 0, n), replicateStepData(st, 1, n)});
    CHECK(merged.find("*BOUNDARY, OP=NEW\n1, 1, 1, 0.5\nLEFT, 2, 2, 0.\n11, 1, 1, 0.5\nLEFT_ABSSC1, 2, 2, 0.\n") != std::string::npos);
    CHECK(merged.find("*DSLOAD, OP=NEW\nTOPS, P, 3.\nTOPS_ABSSC1, P, 3.\n") != std::string::npos);
    CHECK(merged.find("*DLOAD, OP=NEW\n1, P2, 4.\n11, P2, 4.\n") != std::string::npos);
    CHECK(merged.find("0.1, 0.1\n*BOUNDARY") != std::string::npos);  // procedure data once
    // keyword sequences must agree
    CHECK_THROWS(mergeCopySteps({st, "*STEP\n*END STEP\n"}));

    FilIncrement all;
    all.step = 3;
    all.U[2] = {1.0, 2.0};
    all.U[12] = {3.0, 4.0};
    all.RF[12] = {5.0, 6.0};
    FilElementPoint p;
    p.element = 11;
    p.ip = 2;
    p.values["S"] = {7.0};
    all.points.push_back(p);
    const auto c1 = extractCopy(all, 1, n);
    CHECK(c1.step == 3);
    REQUIRE(c1.U.count(2) == 1);
    CHECK(c1.U.at(2)[0] == 3.0);
    CHECK(c1.RF.at(2)[1] == 6.0);
    REQUIRE(c1.points.size() == 1);
    CHECK(c1.points[0].element == 1);
    CHECK(extractCopy(all, 0, n).U.at(2)[0] == 1.0);
    CHECK(extractCopy(all, 0, n).points.empty());
}
