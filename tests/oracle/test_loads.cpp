// Unit tests of the load/BC replay evaluator used by the Abaqus oracle (Vera F6). The Abaqus semantics encoded here
// are additionally self-checked against every nominal job's own U/CF output at run time.
#include "../../src/oracle/abaqus_loads.hpp"

#include <doctest/doctest.h>

using namespace abss;

namespace {
const char* kMesh = R"(*NODE
1, 0., 0.
2, 1., 0.
*ELEMENT, TYPE=T2D2, ELSET=B
1, 1, 2
*SOLID SECTION, ELSET=B, MATERIAL=M
1.
*MATERIAL, NAME=M
*ELASTIC
1000., 0.3
)";
double cload(const abq::LoadState& s, int node, int dof) {
    auto it = s.concentrated.find({node, dof});
    return it == s.concentrated.end() ? 0.0 : it->second;
}
}  // namespace

TEST_CASE("Static step: loads ramp linearly over the step; BCs ramp from zero") {
    const std::string d = std::string(kMesh) +
                          "*STEP\n*STATIC\n0.25, 2.\n*BOUNDARY\n1, 1, 2\n2, 2, 2, 0.4\n*CLOAD\n2, 1, 100.\n*END STEP\n";
    const auto deck = inp::Deck::fromString(d, "mem.inp");
    abq::LoadEvaluator ev(deck);
    const auto s = ev.evaluate(0, 0.5, 0.5);  // tau / T = 0.25
    CHECK(cload(s, 1, 1) == doctest::Approx(25.0));
    CHECK(s.bc.at({1, 2}) == doctest::Approx(0.1));
    CHECK(s.bc.at({0, 1}) == doctest::Approx(0.0));
}

TEST_CASE("Two steps: OP=MOD ramps from the previous value; untouched loads stay; OP=NEW ramps removed loads down") {
    const std::string d = std::string(kMesh) +
                          "*STEP\n*STATIC\n0.5, 1.\n*BOUNDARY\n1, 1, 2\n*CLOAD\n2, 1, 100.\n2, 2, 10.\n*END STEP\n"
                          "*STEP\n*STATIC\n0.5, 1.\n*CLOAD\n2, 1, 300.\n*END STEP\n"
                          "*STEP\n*STATIC\n0.5, 1.\n*CLOAD, OP=NEW\n2, 2, 40.\n*END STEP\n";
    const auto deck = inp::Deck::fromString(d, "mem.inp");
    abq::LoadEvaluator ev(deck);
    const auto s2 = ev.evaluate(1, 0.5, 1.5);
    CHECK(cload(s2, 1, 1) == doctest::Approx(200.0));  // 100 -> 300, half way
    CHECK(cload(s2, 1, 2) == doctest::Approx(10.0));   // propagated, constant
    const auto s3 = ev.evaluate(2, 0.5, 2.5);
    CHECK(cload(s3, 1, 1) == doctest::Approx(150.0));  // removed by OP=NEW: 300 ramps down to 0
    CHECK(cload(s3, 1, 2) == doctest::Approx(25.0));   // redefined: 10 -> 40, half way
}

TEST_CASE("TABULAR amplitude in step time scales the magnitude") {
    const std::string d = std::string(kMesh) +
                          "*AMPLITUDE, NAME=A\n0., 0., 0.5, 0.6, 1., 1.\n"
                          "*STEP\n*STATIC, DIRECT\n0.1, 1.\n*BOUNDARY\n1, 1, 2\n*BOUNDARY, AMPLITUDE=A\n2, 1, 1, 0.05\n*END STEP\n";
    const auto deck = inp::Deck::fromString(d, "mem.inp");
    abq::LoadEvaluator ev(deck);
    CHECK(ev.evaluate(0, 0.25, 0.25).bc.at({1, 1}) == doctest::Approx(0.05 * 0.3));
    CHECK(ev.evaluate(0, 0.75, 0.75).bc.at({1, 1}) == doctest::Approx(0.05 * 0.8));
}

TEST_CASE("Unsupported replay semantics are refused, not approximated") {
    const std::string follower = std::string(kMesh) +
                                 "*STEP, NLGEOM\n*STATIC\n*BOUNDARY\n1, 1, 2\n*CLOAD, FOLLOWER\n2, 1, 1.\n*END STEP\n";
    CHECK_THROWS(abq::LoadEvaluator(inp::Deck::fromString(follower, "f.inp")));
    const std::string riks = std::string(kMesh) + "*STEP\n*STATIC, RIKS\n*BOUNDARY\n1, 1, 2\n*END STEP\n";
    CHECK_THROWS(abq::LoadEvaluator(inp::Deck::fromString(riks, "r.inp")));
    const std::string dyn = std::string(kMesh) + "*STEP\n*DYNAMIC\n0.1, 1.\n*BOUNDARY\n1, 1, 2\n*END STEP\n";
    CHECK_THROWS(abq::LoadEvaluator(inp::Deck::fromString(dyn, "d.inp")));
}
