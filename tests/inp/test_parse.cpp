// Parser tests: lexing, round trip, includes, mesh, sets, steps, boundaries.
#include <doctest/doctest.h>

#include "inp_test_util.hpp"

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cmath>
#include <limits>
#include <locale>
#include <random>

using namespace abss;
using namespace abss::inp;
using namespace abss_test;

TEST_CASE("parseNumber accepts Abaqus number variants") {
    CHECK(parseNumber("1.").value() == 1.0);
    CHECK(parseNumber("1e3").value() == 1000.0);
    CHECK(parseNumber("1.D3").value() == 1000.0);
    CHECK(parseNumber("1.5d-2").value() == 0.015);
    CHECK(parseNumber("-.5").value() == -0.5);
    CHECK(parseNumber("+2.5").value() == 2.5);
    CHECK(parseNumber("  3  ").value() == 3.0);
    CHECK(parseNumber("2.1E+05").value() == 210000.0);
    CHECK(parseNumber("0").value() == 0.0);
    CHECK_FALSE(parseNumber(""));
    CHECK_FALSE(parseNumber("   "));
    CHECK_FALSE(parseNumber("abc"));
    CHECK_FALSE(parseNumber("1.0.0"));
    CHECK_FALSE(parseNumber("inf"));
    CHECK_FALSE(parseNumber("nan"));
    CHECK_FALSE(parseNumber("1e"));
    CHECK_FALSE(parseNumber("<E>"));
    CHECK_FALSE(parseNumber("1,5"));
}

TEST_CASE("formatNumber round-trips exactly and uses an E exponent") {
    std::mt19937_64 rng(12345);
    std::uniform_real_distribution<double> mant(-10.0, 10.0);
    std::uniform_int_distribution<int> ex(-300, 300);
    std::vector<double> values = {0.0, -0.0, 1.0, 0.1, 1.0 / 3.0, 210000.0, -2.5e-7, 1e-300, 5e-324,
                                  std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest(), 0.3};
    for (int i = 0; i < 2000; ++i) values.push_back(mant(rng) * std::pow(10.0, ex(rng)));
    for (double v : values) {
        const std::string s = formatNumber(v);
        CAPTURE(s);
        REQUIRE(parseNumber(s).has_value());
        CHECK(parseNumber(s).value() == v);
        CHECK(std::signbit(parseNumber(s).value()) == std::signbit(v));
        CHECK(s.find('E') != std::string::npos);
        CHECK(s.find('e') == std::string::npos);
        CHECK(s.find('.') != std::string::npos);
        CHECK(s.size() <= 25);
    }
    CHECK(formatNumber(210000.0) == "2.1E+05");
    CHECK(formatNumber(1.0) == "1.0E+00");
    CHECK_THROWS(formatNumber(std::numeric_limits<double>::infinity()));
    CHECK_THROWS(formatNumber(std::nan("")));
}

TEST_CASE("number parsing and formatting are locale independent") {
    const char* candidates[] = {"de_DE.UTF-8", "de_DE", "German_Germany.1252", "German", "fr_FR.UTF-8", "French"};
    std::string old = std::setlocale(LC_ALL, nullptr) ? std::setlocale(LC_ALL, nullptr) : "C";
    bool switched = false;
    for (const char* c : candidates)
        if (std::setlocale(LC_ALL, c)) {
            switched = true;
            break;
        }
    // Also try the C++ global locale (may be unsupported by this runtime).
    try {
        std::locale::global(std::locale(""));
    } catch (...) {
    }
    MESSAGE("decimal-comma locale active: " << (switched ? "yes" : "no (test still checks classic behaviour)"));
    CHECK(parseNumber("1.5").value() == 1.5);
    CHECK(formatNumber(1.5) == "1.5E+00");
    const Deck d = Deck::fromString("*NODE\n1, 0.25, 1.5\n", "loc.inp");
    CHECK(d.mesh().coord(0)[0] == 0.25);
    CHECK(d.mesh().coord(0)[1] == 1.5);
    std::setlocale(LC_ALL, old.c_str());
    std::locale::global(std::locale::classic());
}

TEST_CASE("splitFields") {
    CHECK(splitFields("1, 2,3,") == std::vector<std::string>{"1", "2", "3"});
    CHECK(splitFields("a,,b") == std::vector<std::string>{"a", "", "b"});
    CHECK(splitFields("  x  ") == std::vector<std::string>{"x"});
    CHECK(splitFields("").empty());
    CHECK(splitFields("1, 2\r") == std::vector<std::string>{"1", "2"});
}

TEST_CASE("round trip is byte identical for LF, CRLF, mixed, BOM and missing final newline") {
    const std::string body = "*HEADING\nround trip\n** comment\n*NODE, NSET=N\n1, 0., 0.\n\n   2,  1.0 , 0.\n";
    std::vector<std::string> variants;
    variants.push_back(body);
    std::string crlf;
    for (char c : body) {
        if (c == '\n') crlf += "\r\n";
        else crlf += c;
    }
    variants.push_back(crlf);
    variants.push_back(body.substr(0, body.size() - 1));          // no final newline
    variants.push_back(crlf.substr(0, crlf.size() - 2));          // CRLF, no final newline
    variants.push_back("*NODE\r\n1, 0., 0.\n2, 1., 0.\r\n");      // mixed terminators
    variants.push_back("\xEF\xBB\xBF*NODE\n1, 0., 0.\n");         // UTF-8 BOM
    variants.push_back("");                                         // empty
    variants.push_back("\n\n");
    for (const auto& v : variants) {
        CAPTURE(v);
        const Deck d = Deck::fromString(v, "rt.inp");
        CHECK(d.fileText(0) == v);
        // Patching nothing returns the same bytes too.
        CHECK(applyEdits(d, {})[0] == v);
    }
    // BOM deck still parses its first keyword.
    const Deck bom = Deck::fromString(variants[5], "bom.inp");
    CHECK(bom.mesh().nodeCount() == 1);
    // CRLF detection.
    CHECK(Deck::fromString(crlf, "c.inp").files()[0].lineEnding == "\r\n");
    CHECK(Deck::fromString(body, "l.inp").files()[0].lineEnding == "\n");
}

TEST_CASE("keywords are case-insensitive, parameters normalized, continuation lines joined") {
    const std::string text =
        "*Heading\n  My   title, with commas\n"
        "*node, nset=abc\n1, 0, 0, 0\n"
        "** comment between data\n2, 1, 0, 0\n"
        "*Solid  Section ,  elset = PLATE, material=Mat\n"
        "*ELEMENT, TYPE=T3D2,\n"
        "** comment inside the card\n"
        "   ELSET=bars\n"
        "1, 1, 2\n"
        "*Step, name=\"My Step\", nlgeom\n*static\n*end step\n";
    const Deck d = Deck::fromString(text, "kw.inp");
    CHECK(d.heading() == "My   title, with commas");
    const auto& b = d.blocks();
    REQUIRE(b.size() == 7);
    CHECK(b[1].keyword == "*NODE");
    CHECK(b[1].param("NSET").value() == "abc");
    CHECK(b[1].param("nset").value() == "abc");
    CHECK(b[1].data.size() == 2);
    CHECK(b[1].data[1].lineNumber == 6);
    CHECK(b[2].keyword == "*SOLID SECTION");
    CHECK(b[2].param("ELSET").value() == "PLATE");
    CHECK(b[2].param("MATERIAL").value() == "Mat");
    CHECK(b[3].keyword == "*ELEMENT");
    CHECK(b[3].keywordLineCount == 3);
    CHECK(b[3].param("ELSET").value() == "bars");
    CHECK(b[3].data.size() == 1);
    CHECK(b[3].data[0].lineNumber == 11);
    REQUIRE(d.steps().size() == 1);
    CHECK(d.steps()[0].name == "My Step");
    CHECK(d.steps()[0].nlgeom);
    CHECK(d.steps()[0].procedure == "*STATIC");
    CHECK(d.elementSets().count("BARS") == 1);
    CHECK(d.nodeSets().at("ABC").size() == 2);
    CHECK(d.mesh().spatialDim == 3);
}

TEST_CASE("includes: quoted paths with spaces, data-only include, including-file fallback, missing include") {
    TempDir td("inc");
    writeFile(td.file("root deck.inp"),
              "*HEADING\nincludes\n"
              "*NODE, NSET=ALL\n"
              "1, 0., 0.\n"
              "*INCLUDE, INPUT=\"sub dir/more nodes.inp\"\n"
              "3, 2., 0.\n"
              "*INCLUDE, INPUT=sub dir/elements.inp\n"
              "*INCLUDE, INPUT=does_not_exist.inp\n"
              "*MATERIAL, NAME=M\n*ELASTIC\n100., 0.25\n");
    // Data-only include continues the *NODE block of the including file.
    writeFile(td.file("sub dir/more nodes.inp"), "** nodes from an include\n2, 1., 0.\n");
    // Nested include that exists only next to the including file (sub dir/): found via the fallback with a
    // warning (Abaqus itself resolves against the job directory), CRLF endings.
    writeFile(td.file("sub dir/elements.inp"), "*ELEMENT, TYPE=T2D2, ELSET=BARS\r\n1, 1, 2\r\n*INCLUDE, INPUT=nested.inp\r\n");
    writeFile(td.file("sub dir/nested.inp"), "2, 2, 3\n");
    const Deck d = Deck::load(td.file("root deck.inp"));
    REQUIRE(d.files().size() == 4);
    CHECK(d.mesh().nodeCount() == 3);
    CHECK(d.mesh().elementCount() == 2);
    CHECK(d.nodeSets().at("ALL").size() == 3);
    CHECK(d.elementSets().at("BARS").size() == 2);
    // Provenance of the included node data line.
    const KeywordBlock& node = d.blocks()[1];
    REQUIRE(node.keyword == "*NODE");
    REQUIRE(node.data.size() == 3);
    CHECK(node.data[1].fileIndex == 1);
    CHECK(node.data[1].lineNumber == 2);
    CHECK(node.data[2].fileIndex == 0);
    CHECK(d.files()[1].includedFromFile == 0);
    CHECK(d.files()[1].includedFromLine == 5);
    CHECK(d.files()[3].includedFromFile == 2);
    CHECK(d.files()[2].lineEnding == "\r\n");
    // Missing include is reported, not fatal.
    REQUIRE(d.missingIncludes().size() == 1);
    CHECK(d.missingIncludes()[0] == "does_not_exist.inp");
    CHECK(countErrors(d) == 1);
    CHECK(hasDiagnostic(d, "does_not_exist.inp"));
    CHECK(hasDiagnostic(d, "only relative to the including file"));
    // Every file round-trips.
    for (int i = 0; i < static_cast<int>(d.files().size()); ++i) CHECK(d.fileText(i) == readFile(d.files()[i].path));
    // Discovery finds the included material.
    const auto ps = discoverParameters(d);
    CHECK(findParam(ps, "M: E") != nullptr);
}

TEST_CASE("includes: job-directory resolution of nested includes and cycle detection") {
    TempDir td("cycle");
    writeFile(td.file("a.inp"), "*NODE\n1, 0., 0.\n*INCLUDE, INPUT=b.inp\n");
    writeFile(td.file("b.inp"), "*NODE\n2, 1., 0.\n*INCLUDE, INPUT=a.inp\n");
    const Deck d = Deck::load(td.file("a.inp"));
    CHECK(d.files().size() == 2);
    CHECK(d.mesh().nodeCount() == 2);
    CHECK(hasDiagnostic(d, "include cycle"));
    CHECK(d.missingIncludes().empty());

    // sub/x.inp includes "y.inp": Abaqus 2024 resolves it against the job (root) directory, so ./y.inp wins
    // over sub/y.inp without any diagnostic.
    writeFile(td.file("root.inp"), "*INCLUDE, INPUT=sub/x.inp\n");
    writeFile(td.file("sub/x.inp"), "*INCLUDE, INPUT=y.inp\n");
    writeFile(td.file("y.inp"), "*NODE\n7, 1., 2.\n");
    writeFile(td.file("sub/y.inp"), "*NODE\n8, 1., 2.\n");
    const Deck r = Deck::load(td.file("root.inp"));
    REQUIRE(r.mesh().nodeCount() == 1);
    CHECK(r.mesh().nodeKeys[0].label == 7);
    CHECK(r.missingIncludes().empty());
    CHECK(r.diagnostics().empty());
}

TEST_CASE("Deck::load throws for a missing root file") {
    CHECK_THROWS_AS(Deck::load("Z:/definitely/not/here.inp"), std::runtime_error);
}

TEST_CASE("C3D20 element data continued over two lines (with and without trailing comma)") {
    std::string text = "*NODE\n";
    for (int i = 1; i <= 32; ++i) text += std::to_string(i) + ", " + std::to_string(i) + "., 0., 0.\n";
    text +=
        "*ELEMENT, TYPE=C3D20R, ELSET=QUAD\n"
        "1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,\n"
        "   16, 17, 18, 19, 20\n"
        "2, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27\n"
        "   28, 29, 30, 31, 32\n"
        "*ELEMENT, TYPE=C3D8, ELSET=LIN\n"
        "3, 1, 2, 3, 4, 5, 6, 7, 8,\n"
        "4, 2, 3, 4, 5, 6, 7, 8, 9\n";
    const Deck d = Deck::fromString(text, "c3d20.inp");
    REQUIRE(d.mesh().elementCount() == 4);
    const auto& e1 = d.mesh().elements[0];
    CHECK(e1.label == 1);
    CHECK(e1.conn.size() == 20);
    CHECK(e1.info.shape == ElementShape::Hex20);
    CHECK(d.mesh().nodeKeys[e1.conn[19]].label == 20);
    const auto& e2 = d.mesh().elements[1];
    CHECK(e2.label == 2);
    CHECK(e2.conn.size() == 20);
    CHECK(d.mesh().nodeKeys[e2.conn[0]].label == 13);
    CHECK(d.mesh().nodeKeys[e2.conn[19]].label == 32);
    // A complete C3D8 line ending in a comma does not swallow the next element.
    CHECK(d.mesh().elements[2].conn.size() == 8);
    CHECK(d.mesh().elements[3].label == 4);
    CHECK(d.elementSets().at("QUAD").size() == 2);
    CHECK(countErrors(d) == 0);
}

TEST_CASE("user elements: node count from *USER ELEMENT, comma continuation for unknown types") {
    const std::string text =
        "*NODE\n1,0,0\n2,1,0\n3,1,1\n4,0,1\n"
        "*USER ELEMENT, TYPE=U7, NODES=4, COORDINATES=2, PROPERTIES=1\n1, 2\n"
        "*ELEMENT, TYPE=U7, ELSET=UE\n1, 1, 2,\n 3, 4\n"
        "*ELEMENT, TYPE=XYZ9, ELSET=ODD\n2, 1, 2,\n3\n";
    const Deck d = Deck::fromString(text, "u.inp");
    REQUIRE(d.mesh().elementCount() == 2);
    CHECK(d.mesh().elements[0].conn.size() == 4);
    CHECK(d.mesh().elements[0].info.family == ElementFamily::User);
    CHECK(d.mesh().elements[0].info.shape == ElementShape::Quad4);
    CHECK(d.mesh().elements[1].conn.size() == 3);
    CHECK(d.mesh().spatialDim == 2);
    CHECK(d.usesUserElement());
    CHECK_FALSE(d.usesUserMaterial());
}

TEST_CASE("parts, instances (translation + rotation), instance-scoped and nested sets") {
    const std::string text =
        "*PART, NAME=P\n"
        "*NODE\n1, 0., 0.\n2, 1., 0.\n3, 1., 1.\n4, 0., 1.\n"
        "*ELEMENT, TYPE=CPS4, ELSET=E\n1, 1, 2, 3, 4\n"
        "*NSET, NSET=EDGE, GENERATE\n2, 3, 1\n"
        "*NSET, NSET=ALLP\nEDGE, 1, 4\n"
        "*SOLID SECTION, ELSET=E, MATERIAL=M\n1.\n"
        "*END PART\n"
        "*ASSEMBLY, NAME=A\n"
        "*INSTANCE, NAME=I1, PART=P\n10., 0., 0.\n*END INSTANCE\n"
        "*INSTANCE, NAME=I2, PART=P\n0., 0., 0.\n0., 0., 0., 0., 0., 1., 90.\n*END INSTANCE\n"
        "*NSET, NSET=ASET, INSTANCE=I1\n1, EDGE\n"
        "*NSET, NSET=BOTH\nI1.EDGE, I2.EDGE, I2.4\n"
        "*ELSET, ELSET=ALLEL\nI1.E, I2.E\n"
        "*END ASSEMBLY\n"
        "*MATERIAL, NAME=M\n*ELASTIC\n1000., 0.3\n";
    const Deck d = Deck::fromString(text, "parts.inp");
    const Mesh& m = d.mesh();
    REQUIRE(m.instanceNames.size() == 2);
    CHECK(m.nodeCount() == 8);
    CHECK(m.elementCount() == 2);
    const auto i1n2 = m.findNode({0, 2});
    REQUIRE(i1n2 >= 0);
    CHECK(m.coord(i1n2)[0] == doctest::Approx(11.0));
    // I2 rotated 90 deg about z: (1, 0) -> (0, 1); (1, 1) -> (-1, 1).
    const auto i2n2 = m.findNode({1, 2});
    CHECK(m.coord(i2n2)[0] == doctest::Approx(0.0).epsilon(1e-12));
    CHECK(m.coord(i2n2)[1] == doctest::Approx(1.0));
    const auto i2n3 = m.findNode({1, 3});
    CHECK(m.coord(i2n3)[0] == doctest::Approx(-1.0));
    CHECK(d.nodeSets().at("I1.EDGE").size() == 2);
    CHECK(d.nodeSets().at("I2.EDGE").size() == 2);
    CHECK(d.nodeSets().at("I1.ALLP").size() == 4);  // EDGE (2, 3) + 1, 4
    CHECK(d.nodeSets().at("ASET").size() == 3);
    CHECK(d.nodeSets().at("BOTH").size() == 5);
    CHECK(d.elementSets().at("ALLEL").size() == 2);
    CHECK(m.elements[1].instance == 1);
    CHECK(m.spatialDim == 2);
    // Node source of an instanced node is the part's *NODE line (shared by both instances).
    CHECK(d.nodeDefinition(i1n2).line == d.nodeDefinition(i2n2).line);
    CHECK(d.nodeDefinition(i1n2).line == 4);
    // resolveNodes: label in scope, instance-qualified label, set names.
    CHECK(d.resolveNodes("2", "I1") == std::vector<std::int32_t>{i1n2});
    CHECK(d.resolveNodes("i2.2", "") == std::vector<std::int32_t>{i2n2});
    CHECK(d.resolveNodes("EDGE", "I2").size() == 2);
    CHECK(d.resolveNodes("both", "").size() == 5);
    CHECK(d.resolveNodes("nope", "").empty());
    CHECK(countErrors(d) == 0);
}

TEST_CASE("instance transform order matches Abaqus: translation first, then rotation") {
    // Reference: Abaqus 2024 *NODE PRINT COORD of this exact model gave node 1 -> (6.1e-16, 10, 0) and
    // node 2 -> (6.7e-16, 11, 0).
    const std::string text =
        "*PART, NAME=P\n*NODE\n1, 0., 0., 0.\n2, 1., 0., 0.\n*ELEMENT, TYPE=T3D2, ELSET=B\n1, 1, 2\n*END PART\n"
        "*ASSEMBLY, NAME=A\n*INSTANCE, NAME=I, PART=P\n10., 0., 0.\n0., 0., 0., 0., 0., 1., 90.\n*END INSTANCE\n"
        "*END ASSEMBLY\n";
    const Deck d = Deck::fromString(text, "rot.inp");
    const auto& m = d.mesh();
    CHECK(m.coord(m.findNode({0, 1}))[0] == doctest::Approx(0.0).epsilon(1e-12));
    CHECK(m.coord(m.findNode({0, 1}))[1] == doctest::Approx(10.0));
    CHECK(m.coord(m.findNode({0, 2}))[1] == doctest::Approx(11.0));
}

TEST_CASE("large mesh: 64000 C3D8 elements parse, discover and patch") {
    const int n = 40, nn = n + 1;
    std::string text = "*NODE, NSET=ALLN\n";
    text.reserve(8'000'000);
    auto id = [&](int i, int j, int k) { return 1 + i + nn * j + nn * nn * k; };
    for (int k = 0; k < nn; ++k)
        for (int j = 0; j < nn; ++j)
            for (int i = 0; i < nn; ++i)
                text += std::to_string(id(i, j, k)) + ", " + std::to_string(i) + ".5, " + std::to_string(j) + ".25, " +
                        std::to_string(k) + ".\n";
    text += "*ELEMENT, TYPE=C3D8R, ELSET=ALLE\n";
    int e = 1;
    for (int k = 0; k < n; ++k)
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
                text += std::to_string(e++) + ", " + std::to_string(id(i, j, k)) + ", " + std::to_string(id(i + 1, j, k)) +
                        ", " + std::to_string(id(i + 1, j + 1, k)) + ", " + std::to_string(id(i, j + 1, k)) + ", " +
                        std::to_string(id(i, j, k + 1)) + ", " + std::to_string(id(i + 1, j, k + 1)) + ", " +
                        std::to_string(id(i + 1, j + 1, k + 1)) + ", " + std::to_string(id(i, j + 1, k + 1)) + "\n";
    text += "*NSET, NSET=BOT, GENERATE\n1, " + std::to_string(nn * nn) + ", 1\n";
    text += "*MATERIAL, NAME=M\n*ELASTIC\n1000., 0.3\n*STEP\n*STATIC\n*BOUNDARY\nBOT, ENCASTRE\n*END STEP\n";
    const auto t0 = std::chrono::steady_clock::now();
    const Deck d = Deck::fromString(text, "large.inp");
    const auto ps = discoverParameters(d);
    const auto out = applyEdits(d, editsForParameter(*findParam(ps, "Move set BOT in Z"), 0.5));
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("large mesh parse+discover+patch: " << secs << " s");
    CHECK(d.mesh().nodeCount() == nn * nn * nn);
    CHECK(d.mesh().elementCount() == n * n * n);
    CHECK(d.fileText(0) == text);
    CHECK(d.boundaries()[0].nodes.size() == static_cast<std::size_t>(nn * nn));
    CHECK(out[0].size() > text.size());
    CHECK(countErrors(d) == 0);
}

TEST_CASE("GENERATE sets and *NSET, ELSET=") {
    std::string text = "*NODE\n";
    for (int i = 1; i <= 9; ++i) text += std::to_string(i) + ", " + std::to_string(i) + ".\n";
    text += "*ELEMENT, TYPE=T2D2\n1, 1, 2\n2, 2, 3\n3, 8, 9\n"
            "*NSET, NSET=ODD, GENERATE\n1, 9, 2\n"
            "*NSET, NSET=ONE, GENERATE\n4, 4\n"
            "*ELSET, ELSET=EG, GENERATE\n1, 3, 2\n"
            "*NSET, NSET=FROMEL, ELSET=EG\n";
    const Deck d = Deck::fromString(text, "gen.inp");
    CHECK(d.nodeSets().at("ODD").size() == 5);
    CHECK(d.nodeSets().at("ONE").size() == 1);
    CHECK(d.elementSets().at("EG").size() == 2);
    CHECK(d.nodeSets().at("FROMEL").size() == 4);  // nodes 1, 2, 8, 9
}

TEST_CASE("steps: procedures, DIRECT incrementation, defaults, perturbation") {
    const std::string text =
        "*STEP, NAME=S1, NLGEOM=YES, INC=100\n*STATIC, DIRECT\n0.1, 2.\n*END STEP\n"
        "*STEP, PERTURBATION\n*STATIC\n*END STEP\n"
        "*STEP, NLGEOM=NO\n*STATIC\n0.2, 1., 1e-6, 0.5\n*END STEP\n"
        "*STEP\n*FREQUENCY\n5\n*END STEP\n";
    const Deck d = Deck::fromString(text, "steps.inp");
    REQUIRE(d.steps().size() == 4);
    const auto& s1 = d.steps()[0];
    CHECK(s1.name == "S1");
    CHECK(s1.nlgeom);
    CHECK(s1.fixedIncrementation);
    CHECK(s1.initialIncrement == 0.1);
    CHECK(s1.timePeriod == 2.0);
    CHECK(s1.blockIndex == 0);
    CHECK(s1.endBlockIndex == 2);
    const auto& s2 = d.steps()[1];
    CHECK(s2.name == "Step-2");
    CHECK(s2.perturbation);
    CHECK(s2.initialIncrement == 1.0);
    CHECK(s2.timePeriod == 1.0);
    CHECK_FALSE(s2.fixedIncrementation);
    const auto& s3 = d.steps()[2];
    CHECK_FALSE(s3.nlgeom);
    CHECK(s3.initialIncrement == 0.2);
    CHECK(s3.minIncrement == 1e-6);
    CHECK(s3.maxIncrement == 0.5);
    CHECK(d.steps()[3].procedure == "*FREQUENCY");
    CHECK(d.blocks()[1].stepIndex == 0);
    CHECK(d.blocks()[5].stepIndex == 1);
}

TEST_CASE("boundary conditions: forms, named types, parameters") {
    const std::string text =
        "*NODE, NSET=ALL\n1, 0., 0., 0.\n2, 1., 0., 0.\n3, 2., 0., 0.\n"
        "*NSET, NSET=LEFT\n1\n"
        "*BOUNDARY\nLEFT, ENCASTRE\n"
        "*STEP, NAME=S\n*STATIC\n"
        "*BOUNDARY, OP=NEW, AMPLITUDE=RAMP2, TYPE=VELOCITY\n"
        "2, XSYMM\n3, ZSYMM\n2, YASYMM\n"
        "3, 1, , 0.5\n"
        "2, 2\n"
        "ALL, 3, 3, -1.25\n"
        "1, PINNED\n"
        "*END STEP\n";
    const Deck d = Deck::fromString(text, "bc.inp");
    const auto bcs = d.boundaries();
    REQUIRE(bcs.size() == 11);
    CHECK(bcs[0].firstDof == 1);
    CHECK(bcs[0].lastDof == 6);
    CHECK(bcs[0].stepIndex == -1);
    CHECK(bcs[0].op == "MOD");
    CHECK(bcs[0].type == "DISPLACEMENT");
    CHECK(bcs[0].nodes.size() == 1);
    // XSYMM -> 1, 5-6
    CHECK(bcs[1].firstDof == 1);
    CHECK(bcs[1].lastDof == 1);
    CHECK(bcs[2].firstDof == 5);
    CHECK(bcs[2].lastDof == 6);
    CHECK(bcs[1].op == "NEW");
    CHECK(bcs[1].amplitude == "RAMP2");
    CHECK(bcs[1].type == "VELOCITY");
    CHECK(bcs[1].stepIndex == 0);
    // ZSYMM -> 3-5
    CHECK(bcs[3].firstDof == 3);
    CHECK(bcs[3].lastDof == 5);
    // YASYMM -> 1, 3, 5
    CHECK(bcs[4].firstDof == 1);
    CHECK(bcs[5].firstDof == 3);
    CHECK(bcs[6].firstDof == 5);
    // "3, 1, , 0.5": empty last dof -> first, magnitude in field 3.
    CHECK(bcs[7].firstDof == 1);
    CHECK(bcs[7].lastDof == 1);
    CHECK(bcs[7].magnitude == 0.5);
    CHECK(bcs[7].valueRef.field == 3);
    CHECK(bcs[7].valueRef.line == 15);
    CHECK(bcs[7].valueRef.keyword == "*BOUNDARY");
    // "2, 2": no magnitude.
    CHECK(bcs[8].firstDof == 2);
    CHECK(bcs[8].lastDof == 2);
    CHECK(bcs[8].magnitude == 0.0);
    CHECK(bcs[8].valueRef.field == -1);
    CHECK(bcs[9].nodes.size() == 3);
    CHECK(bcs[9].magnitude == -1.25);
    CHECK(bcs[10].lastDof == 3);
}

TEST_CASE("*PARAMETER is recorded with a partial-support warning") {
    const std::string text = "*PARAMETER\nthick = 2.5\nE = 2.1e5\nname = 'abc'\n*NODE\n1, 0., 0.\n";
    const Deck d = Deck::fromString(text, "param.inp");
    CHECK(hasDiagnostic(d, "partially supported"));
    const auto ps = discoverParameters(d);
    const auto* t = findParam(ps, "*PARAMETER thick");
    REQUIRE(t != nullptr);
    CHECK(t->nominal == 2.5);
    CHECK(t->category == ParamCategory::Custom);
    CHECK(findParam(ps, "*PARAMETER name") == nullptr);
    // Edit the parameter value: only the right-hand side changes.
    const auto out = applyEdits(d, editsForParameter(*t, 3.0));
    CHECK(out[0] == "*PARAMETER\nthick = 3.0E+00\nE = 2.1e5\nname = 'abc'\n*NODE\n1, 0., 0.\n");
}

TEST_CASE("diagnostics for malformed input do not throw") {
    const std::string text =
        "1, 2, 3\n"                                 // data before any keyword
        "*ELEMENT, TYPE=CPS4\n1, 1, 2, 3, 4\n"      // undefined nodes
        "*NSET, NSET=S\n99\n"                       // undefined node in set
        "*STEP\n*STATIC\n";                         // no *END STEP
    const Deck d = Deck::fromString(text, "bad.inp");
    CHECK(hasDiagnostic(d, "before any keyword"));
    CHECK(hasDiagnostic(d, "undefined node"));
    CHECK(hasDiagnostic(d, "no *END STEP"));
    CHECK(d.mesh().elementCount() == 1);
    CHECK(d.mesh().elements[0].conn.empty());
}
