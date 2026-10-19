// ABSS INP: structure-preserving Abaqus input deck model (lossless round trip, include provenance).
#pragma once

#include "abss/core/mesh.hpp"
#include "abss/core/parameter.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace abss::inp {

struct Diagnostic {
    enum class Level { Info, Warning, Error } level = Level::Warning;
    std::string file;
    int line = 0;
    std::string message;
};

/// One physical line of one source file, kept verbatim (without the line terminator).
struct SourceLine {
    int fileIndex = 0;
    int lineNumber = 0;  ///< 1-based
    std::string text;
};

struct SourceFile {
    std::string path;          ///< absolute path
    std::string lineEnding;    ///< "\r\n" or "\n" as found
    int includedFromFile = -1; ///< index of including file (-1 root)
    int includedFromLine = 0;
    std::vector<std::string> lines;
    bool finalNewline = true;  ///< file ends with a line terminator (needed for byte-identical round trip)
};

/// Physical location (file index, 1-based line) of a definition; fileIndex -1 when unknown.
struct LineLoc {
    int fileIndex = -1;
    int line = 0;
};

/// A keyword card with its parameters and data lines. Data may span lines and (rarely) files.
struct KeywordBlock {
    std::string keyword;   ///< normalized upper case with '*' and single spaces, e.g. "*SOLID SECTION"
    /// Parameters in source order: key upper-case, value as written (trimmed); flag parameters have empty value.
    std::vector<std::pair<std::string, std::string>> params;
    int fileIndex = 0;
    int line = 0;          ///< first line of the keyword card (continuation lines follow)
    int keywordLineCount = 1;
    std::vector<SourceLine> data;  ///< data lines (comments excluded), with provenance
    int stepIndex = -1;    ///< -1 = model data, otherwise 0-based step
    std::string partName;  ///< enclosing *PART name ("" when none)
    std::string instanceName; ///< enclosing *INSTANCE name ("" when none)

    std::optional<std::string> param(const std::string& keyUpper) const;
    bool hasParam(const std::string& keyUpper) const;
};

/// Split a data line into trimmed comma-separated fields (trailing empty field from final comma dropped).
std::vector<std::string> splitFields(const std::string& line);
/// Parse Abaqus floating-point token (accepts "1.", "1e3", "1.D3", "-.5"). Empty optional on failure.
std::optional<double> parseNumber(const std::string& token);

struct StepInfo {
    std::string name;
    std::string procedure;   ///< "*STATIC", "*DYNAMIC", ...
    bool nlgeom = false;
    bool perturbation = false;
    double timePeriod = 1.0;
    double initialIncrement = 1.0;
    double minIncrement = 0.0;
    double maxIncrement = 1.0;
    bool fixedIncrementation = false;  ///< *STATIC, DIRECT
    int blockIndex = -1;               ///< index of *STEP block
    int endBlockIndex = -1;            ///< index of *END STEP block
};

/// A *BOUNDARY data entry resolved to the nodes it constrains.
struct BoundaryEntry {
    int blockIndex = -1;
    int stepIndex = -1;        ///< -1 = initial conditions in model data
    std::string target;        ///< node label or nset name as written
    int firstDof = 1, lastDof = 1;
    double magnitude = 0.0;    ///< prescribed value (0 if omitted)
    std::string amplitude;
    std::string op;            ///< "MOD" or "NEW"
    std::string type;          ///< "DISPLACEMENT", "VELOCITY", ...
    std::vector<std::int32_t> nodes;  ///< mesh node indices
    SourceRef valueRef;        ///< token location of the magnitude (field index valid only if present)
};

class Deck {
public:
    /// Parse a root INP, following *INCLUDE, INPUT= recursively (cycle-safe). Relative paths are resolved like
    /// Abaqus does, against the job directory (= root deck directory), then against the including file's
    /// directory as a fallback with a warning.
    static Deck load(const std::string& path);
    /// Parse from memory (path used for provenance and relative includes).
    static Deck fromString(const std::string& text, const std::string& virtualPath);

    const std::vector<SourceFile>& files() const { return files_; }
    const std::vector<KeywordBlock>& blocks() const { return blocks_; }
    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }
    const std::vector<StepInfo>& steps() const { return steps_; }
    const std::string& heading() const { return heading_; }
    /// Missing dependencies discovered (includes not found, user subroutine needs, ...).
    std::vector<std::string> missingIncludes() const;
    bool usesUserMaterial() const;
    bool usesUserElement() const;

    /// Flattened mesh (parts instanced through the assembly, with instance translations applied when present).
    const Mesh& mesh() const { return mesh_; }
    /// Node and element sets, keyed "INSTANCE.SETNAME" or "SETNAME" (upper case) -> mesh indices.
    const std::map<std::string, std::vector<std::int32_t>>& nodeSets() const { return nsets_; }
    const std::map<std::string, std::vector<std::int32_t>>& elementSets() const { return elsets_; }
    std::vector<std::int32_t> resolveNodes(const std::string& targetToken, const std::string& scopeInstance) const;

    std::vector<BoundaryEntry> boundaries() const;

    /// Exact original text of all files (round trip must be byte identical).
    std::string fileText(int fileIndex) const;

    /// *NODE data line that defines mesh node `node` (fields 1..3 hold x, y, z). For nodes of an instanced
    /// part this is the line inside the *PART definition (shared by every instance of that part).
    LineLoc nodeDefinition(std::int32_t node) const;

private:
    std::vector<SourceFile> files_;
    std::vector<KeywordBlock> blocks_;
    std::vector<Diagnostic> diagnostics_;
    std::vector<StepInfo> steps_;
    std::string heading_;
    Mesh mesh_;
    std::map<std::string, std::vector<std::int32_t>> nsets_;
    std::map<std::string, std::vector<std::int32_t>> elsets_;
    std::vector<std::string> missing_;
    std::vector<LineLoc> nodeDefs_;
    friend class DeckBuilder;
};

/// Discover differentiable parameter candidates (materials, UMAT/UEL PROPS, loads, BCs, sections, amplitudes,
/// node coordinates as geometry). Opaque PROPS are named by index, never given invented physical names.
std::vector<ParameterInfo> discoverParameters(const Deck& deck);

/// Expert option: a geometry parameter (EditMode::Offset, nominal 0) that moves the given mesh nodes by the
/// parameter value in global direction `direction` (1=x, 2=y, 3=z) by editing their *NODE coordinate tokens.
/// Requires unchanged topology. Throws std::invalid_argument for an empty node list or bad direction.
ParameterInfo makeGeometryParameter(const Deck& deck, const std::vector<std::int32_t>& nodes, int direction,
                                    const std::string& name);

/// Expert override: an explicit custom scalar pointing to one exact numeric token (file/line/field).
/// Throws std::invalid_argument if the location does not hold a numeric token.
ParameterInfo makeCustomParameter(const Deck& deck, const SourceRef& ref, const std::string& name);

/// A token-level edit.
struct TokenEdit {
    SourceRef ref;
    double value = 0.0;
};

/// Format a double for an INP field with round-trip precision (17 significant digits, Abaqus-parsable).
std::string formatNumber(double v);

/// Edits that set parameter `p` to value `newValue` (respecting EditMode/weights).
std::vector<TokenEdit> editsForParameter(const ParameterInfo& p, double newValue);

/// Apply token edits to a copy of all source files: only the edited fields change; everything else is
/// byte-identical. Returns new text per file index. Throws std::runtime_error on an invalid reference.
std::vector<std::string> applyEdits(const Deck& deck, const std::vector<TokenEdit>& edits);

/// Write the edited deck into `outDir`: root file as `<jobName>.inp`, included files copied (edited) next to it
/// with their relative layout preserved. Returns path of the root INP. Original files are never modified.
std::string writeEditedDeck(const Deck& deck, const std::vector<TokenEdit>& edits, const std::string& outDir,
                            const std::string& jobName);

/// Unified-diff style summary of edits (for the reversible-diff UI).
std::string describeEdits(const Deck& deck, const std::vector<TokenEdit>& edits);

}  // namespace abss::inp
