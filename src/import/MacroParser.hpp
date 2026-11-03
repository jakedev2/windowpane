#pragma once
// Macro import: a tiny plug-in style interface so new formats (Mega Hack .mhr, TASBot, ...) only need
// one class that implements IMacroParser and one registerParser() call.

#include "../core/Types.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace wp {

struct ParseResult {
    Macro macro;
    std::string error;                  // empty on success
    std::vector<std::string> warnings;
    std::shared_ptr<AnalysisResult> analysis;   // set by the Windowpane format: the saved analysis, if it had one
    bool keepRaw = false;                       // Windowpane format: `raw` / `imported` come from the file, finalizeMacro must not reset them
    bool ok() const { return error.empty(); }
};

class IMacroParser {
public:
    virtual ~IMacroParser() = default;
    virtual char const* name() const = 0;
    // lower-case extensions including the dot, e.g. ".gdr"; used for the file picker and the folder scan
    virtual std::vector<std::string> extensions() const = 0;
    // cheap content check used to pick a parser when the extension is ambiguous
    virtual bool sniff(std::span<const uint8_t> data) const = 0;
    virtual ParseResult parse(std::span<const uint8_t> data) const = 0;
};

// Parsers are tried in registration order; the first whose sniff() accepts the bytes wins.
void registerParser(std::unique_ptr<IMacroParser> parser);
void registerBuiltInParsers();                       // GDR2 + GDR1 (msgpack and json)
std::vector<std::string> supportedExtensions();

// Reads and parses a file, then runs finalizeMacro(). `error` is set for corrupt / unknown files. `type` is what the
// user picked on the import screen: only the readers that belong to that bot may open the file, and the error messages
// say what to do instead. Auto tries every reader.
ParseResult loadMacroFile(std::filesystem::path const& path, MacroType type = MacroType::Auto);

// For the import screen.
char const* macroTypeName(MacroType t);                       // "xdBot", "zBot", ...
char const* macroTypeSummary(MacroType t);                    // one short line: which files it reads
std::vector<std::string> macroTypeExtensions(MacroType t);    // file-dialog patterns' extensions, with the dot

// Sort, convert to 240 Hz ticks, sanity-check the event stream (alternating press/release, no negative
// ticks, ...). Called by every parser; exposed for the live recorder too.
void finalizeMacro(ParseResult& r);

// A copy of an imported macro with its timing corrected: tick = raw * (240 / fps) * scale + offset (in ticks). With
// `half` the fraction is kept as a half-tick position (sub = 0.5) instead of being rounded to a whole tick.
std::shared_ptr<Macro const> rescaleMacro(Macro const& m, double scale, double offset, bool half);

struct LevelInfo {
    uint32_t id = 0;
    bool platformer = false;
    std::string name;
};

// Compare a macro with the level that is open. Returns an empty string when it is fine, otherwise an
// error that is shown to the user verbatim. Soft problems are appended to `warnings`.
std::string validateAgainstLevel(Macro const& m, LevelInfo const& level, std::vector<std::string>& warnings);

}  // namespace wp
