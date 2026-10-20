// Unit tests of the load/BC replay evaluator used by the Abaqus oracle (Vera F6). The Abaqus semantics encoded here
// are additionally self-checked against every nominal job's own U/CF output at run time.
#include "../../src/oracle/abaqus_loads.hpp"

#include <doctest/doctest.h>

#include <cmath>

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

TEST_CASE("FOLLOWER concentrated loads are replayed as follower loads and kept out of the CF cross-check") {
    const std::string follower = std::string(kMesh) +
                                 "*STEP, NLGEOM\n*STATIC\n0.5, 1.\n*BOUNDARY\n1, 1, 2\n*CLOAD, FOLLOWER\n2, 1, 4.\n*END STEP\n";
    abq::LoadEvaluator ev(inp::Deck::fromString(follower, "f.inp"));
    const auto s = ev.evaluate(0, 0.5, 0.5);
    REQUIRE(s.loads.size() == 1);
    CHECK(s.loads[0].follower);
    CHECK(s.loads[0].fields[2] == inp::formatNumber(2.0));
    CHECK(s.followerDofs.count({1, 1}) == 1);
    CHECK(s.concentrated.empty());
}

TEST_CASE("Analytic amplitudes: SMOOTH STEP, EQUALLY SPACED, PERIODIC, MODULATED, DECAY, VALUE=ABSOLUTE") {
    const std::string d = std::string(kMesh) +
                          "*AMPLITUDE, NAME=SS, DEFINITION=SMOOTH STEP\n0., 0., 1., 1.\n"
                          "*AMPLITUDE, NAME=EQ, DEFINITION=EQUALLY SPACED, FIXED INTERVAL=0.5, BEGIN=0.\n0., 2., 4.\n"
                          "*AMPLITUDE, NAME=PE, DEFINITION=PERIODIC\n1, 3.14159265358979, 0., 1.\n0.5, 0.25\n"
                          "*AMPLITUDE, NAME=MO, DEFINITION=MODULATED\n1., 2., 0., 3., 5.\n"
                          "*AMPLITUDE, NAME=DE, DEFINITION=DECAY\n1., 2., 0.1, 0.5\n"
                          "*AMPLITUDE, NAME=AB, VALUE=ABSOLUTE\n0., 0., 1., 7.\n"
                          "*STEP\n*STATIC, DIRECT\n0.1, 1.\n*BOUNDARY\n1, 1, 2\n"
                          "*CLOAD, AMPLITUDE=SS\n2, 1, 10.\n*CLOAD, AMPLITUDE=EQ\n2, 2, 10.\n*END STEP\n"
                          "*STEP\n*STATIC, DIRECT\n0.1, 1.\n*CLOAD, OP=NEW, AMPLITUDE=PE\n2, 1, 10.\n*CLOAD, AMPLITUDE=MO\n2, 2, 10.\n*END STEP\n"
                          "*STEP\n*STATIC, DIRECT\n0.1, 1.\n*CLOAD, OP=NEW, AMPLITUDE=DE\n2, 1, 10.\n*CLOAD, AMPLITUDE=AB\n2, 2, 123.\n*END STEP\n";
    abq::LoadEvaluator ev(inp::Deck::fromString(d, "amp.inp"));
    const double x = 0.3, ss = x * x * x * (10 - 15 * x + 6 * x * x);
    const auto s1 = ev.evaluate(0, 0.3, 0.3);
    CHECK(cload(s1, 1, 1) == doctest::Approx(10 * ss));
    CHECK(cload(s1, 1, 2) == doctest::Approx(10 * (0.0 + (2.0 - 0.0) * 0.3 / 0.5)));
    const auto s2 = ev.evaluate(1, 0.4, 1.4);
    const double w = 3.14159265358979;
    CHECK(cload(s2, 1, 1) == doctest::Approx(10 * (1.0 + 0.5 * std::cos(w * 0.4) + 0.25 * std::sin(w * 0.4))));
    CHECK(cload(s2, 1, 2) == doctest::Approx(10 * (1.0 + 2.0 * std::sin(3 * 0.4) * std::sin(5 * 0.4))));
    const auto s3 = ev.evaluate(2, 0.6, 2.6);
    CHECK(cload(s3, 1, 1) == doctest::Approx(10 * (1.0 + 2.0 * std::exp(-(0.6 - 0.1) / 0.5))));
    CHECK(cload(s3, 1, 2) == doctest::Approx(7.0 * 0.6));  // ABSOLUTE: the amplitude is the value (123 ignored)
}

TEST_CASE("Constrained sets per step: a BC released by OP=NEW frees its DOF in later steps") {
    const std::string d = std::string(kMesh) +
                          "*STEP\n*STATIC\n0.5, 1.\n*BOUNDARY\n1, 1, 2\n2, 2, 2, 0.1\n*END STEP\n"
                          "*STEP\n*STATIC\n0.5, 1.\n*BOUNDARY, OP=NEW\n1, 1, 2\n*CLOAD\n2, 2, 5.\n*END STEP\n";
    abq::LoadEvaluator ev(inp::Deck::fromString(d, "rel.inp"));
    const auto sets = ev.constrainedByStep();
    REQUIRE(sets.size() == 2);
    CHECK(sets[0].count({1, 2}) == 1);
    CHECK(sets[1].count({1, 2}) == 0);  // released
    CHECK(sets[1].count({0, 1}) == 1);
    CHECK(ev.prescribedDofs().size() == 3);  // union
    CHECK(ev.evaluate(1, 0.5, 1.5).bc.count({1, 2}) == 0);  // no prescribed value once released
    CHECK(ev.stepRamps(1));
}

TEST_CASE("Unsupported replay semantics are refused, not approximated") {
    const std::string riks = std::string(kMesh) + "*STEP\n*STATIC, RIKS\n*BOUNDARY\n1, 1, 2\n*END STEP\n";
    CHECK_THROWS(abq::LoadEvaluator(inp::Deck::fromString(riks, "r.inp")));
    const std::string expl = std::string(kMesh) + "*STEP\n*DYNAMIC, EXPLICIT\n, 1.\n*BOUNDARY\n1, 1, 2\n*END STEP\n";
    CHECK_THROWS(abq::LoadEvaluator(inp::Deck::fromString(expl, "e.inp")));
    const std::string heat = std::string(kMesh) + "*STEP\n*HEAT TRANSFER\n0.1, 1.\n*END STEP\n";
    CHECK_THROWS(abq::LoadEvaluator(inp::Deck::fromString(heat, "h.inp")));
}

TEST_CASE("Implicit dynamic steps are accepted; their loads default to STEP application") {
    const std::string dyn = std::string(kMesh) + "*STEP\n*DYNAMIC\n0.1, 1.\n*BOUNDARY\n1, 1, 2\n*CLOAD\n2, 1, 8.\n*END STEP\n";
    abq::LoadEvaluator ev(inp::Deck::fromString(dyn, "d.inp"));
    CHECK_FALSE(ev.stepRamps(0));
    CHECK(cload(ev.evaluate(0, 0.1, 0.1), 1, 1) == doctest::Approx(8.0));
}
