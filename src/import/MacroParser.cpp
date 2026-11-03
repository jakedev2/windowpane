#include "MacroParser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <matjson.hpp>
#include <unordered_map>

namespace wp {

namespace {

constexpr size_t kMaxFileBytes = 64u * 1024u * 1024u;
constexpr uint64_t kMaxInputs = 5'000'000;

std::vector<std::unique_ptr<IMacroParser>>& registry() {
    static std::vector<std::unique_ptr<IMacroParser>> r;
    return r;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// ------------------------------------------------------------------------------------------------
// Bounds-checked big-endian / LEB128 reader. Every read reports failure instead of running off the
// end, which is what makes truncated or random files produce an error message rather than a hang.
// ------------------------------------------------------------------------------------------------
class Reader {
public:
    explicit Reader(std::span<const uint8_t> d) : m_d(d) {}

    bool ok() const { return m_ok; }
    size_t remaining() const { return m_d.size() - m_pos; }
    bool atEnd() const { return m_pos >= m_d.size(); }

    uint8_t u8() {
        if (!need(1)) return 0;
        return m_d[m_pos++];
    }

    // LEB128 as written by GDReplayFormat's binary_writer (7 bits per byte, low group first)
    uint64_t varint() {
        uint64_t v = 0;
        for (int i = 0; i < 10; ++i) {
            if (!need(1)) return 0;
            const uint8_t b = m_d[m_pos++];
            v |= static_cast<uint64_t>(b & 0x7F) << (7 * i);
            if (!(b & 0x80)) return v;
        }
        m_ok = false;   // more than 10 continuation bytes cannot be a valid 64-bit value
        return 0;
    }

    // GDR2 stores floating point values as big-endian IEEE bytes
    float f32() {
        uint8_t b[4];
        if (!bytes(b, 4)) return 0.f;
        std::reverse(b, b + 4);
        float f;
        std::memcpy(&f, b, 4);
        return f;
    }
    double f64() {
        uint8_t b[8];
        if (!bytes(b, 8)) return 0.0;
        std::reverse(b, b + 8);
        double d;
        std::memcpy(&d, b, 8);
        return d;
    }

    // varint length + bytes
    std::string str(size_t maxLen = 0xFFFF) {
        const uint64_t len = varint();
        if (!m_ok) return {};
        if (len > maxLen || !need(static_cast<size_t>(len))) { m_ok = false; return {}; }
        std::string s(reinterpret_cast<char const*>(m_d.data() + m_pos), static_cast<size_t>(len));
        m_pos += static_cast<size_t>(len);
        return s;
    }

    bool skip(uint64_t n) {
        if (n > remaining()) { m_ok = false; return false; }
        m_pos += static_cast<size_t>(n);
        return true;
    }

    bool bytes(uint8_t* out, size_t n) {
        if (!need(n)) return false;
        std::memcpy(out, m_d.data() + m_pos, n);
        m_pos += n;
        return true;
    }

    // big-endian unsigned integer of 1/2/4/8 bytes (msgpack)
    uint64_t be(size_t n) {
        uint8_t b[8] = {};
        if (!bytes(b, n)) return 0;
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i) v = (v << 8) | b[i];
        return v;
    }

private:
    bool need(size_t n) {
        if (!m_ok || n > m_d.size() - m_pos) { m_ok = false; return false; }
        return true;
    }
    std::span<const uint8_t> m_d;
    size_t m_pos = 0;
    bool m_ok = true;
};

// ------------------------------------------------------------------------------------------------
// GDR2  (magic "GDR", the format written by xdBot / Eclipse / current GDReplayFormat, .gdr2 or .gdr)
// Byte layout verified against maxnut/GDReplayFormat gdr.hpp (branch gdr2, commit d8d2f0b).
// ------------------------------------------------------------------------------------------------
class Gdr2Parser final : public IMacroParser {
public:
    char const* name() const override { return "GDR2"; }
    std::vector<std::string> extensions() const override { return {".gdr2", ".gdr"}; }
    bool sniff(std::span<const uint8_t> d) const override {
        return d.size() >= 4 && d[0] == 'G' && d[1] == 'D' && d[2] == 'R';
    }

    ParseResult parse(std::span<const uint8_t> data) const override {
        ParseResult out;
        Macro& m = out.macro;
        m.format = "GDR2";
        Reader r(data);
        r.skip(3);   // magic

        const uint64_t version = r.varint();
        const std::string inputTag = r.str();
        m.botName.clear();
        std::string author = r.str();
        std::string description = r.str();
        const float duration = r.f32();
        const uint64_t gameVersion = r.varint();
        const double framerate = r.f64();
        const uint64_t seed = r.varint();
        const uint64_t coins = r.varint();
        const bool ldm = r.varint() != 0;
        m.platformer = r.varint() != 0;
        m.botName = r.str();
        const uint64_t botVersion = r.varint();
        m.levelId = static_cast<uint32_t>(r.varint());
        m.levelName = r.str();
        const uint64_t extSize = r.varint();
        r.skip(extSize);
        if (!r.ok()) { out.error = "The file is corrupt or truncated (header could not be read)."; return out; }
        if (version != 2) {
            out.error = "Unsupported GDR version " + std::to_string(version) + " (this reader supports GDR2).";
            return out;
        }
        if (!(framerate > 0.0 && framerate < 100000.0)) {
            out.error = "The macro has an invalid framerate (" + std::to_string(framerate) + ").";
            return out;
        }
        m.sourceTps = framerate;
        (void)author; (void)description; (void)duration; (void)gameVersion; (void)seed; (void)coins;
        (void)ldm; (void)botVersion;

        const uint64_t deathCount = r.varint();
        if (!r.ok() || deathCount > r.remaining()) { out.error = "The file is corrupt (death list size is impossible)."; return out; }
        uint64_t prev = 0;
        for (uint64_t i = 0; i < deathCount; ++i) {
            prev += r.varint();
            m.deaths.push_back(prev);
        }

        const uint64_t inputCount = r.varint();
        uint64_t p1Count = r.varint();
        if (!r.ok() || inputCount > kMaxInputs || inputCount > r.remaining() || p1Count > inputCount) {
            out.error = "The file is corrupt (input counts are impossible).";
            return out;
        }
        if (p1Count == 0 && inputCount > 0) {
            // xdBot writes 0 here for one-player macros although every input is player 1 (a real xdBot macro of a one-player
            // level has 467 inputs, p1 count 0, and a total length that only adds up when they are all read as player 1).
            // Taken literally, every click would be player 2's and do nothing in a one-player level.
            p1Count = inputCount;
            out.warnings.push_back("The file says it has no player 1 inputs; all of its inputs were read as player 1 (some bots, xdBot among them, write it that way).");
        }

        m.inputs.reserve(static_cast<size_t>(inputCount));
        const bool hasInputExt = !inputTag.empty();
        uint64_t frame = 0;
        for (uint64_t i = 0; i < inputCount; ++i) {
            if (i == p1Count) frame = 0;   // player 2 section restarts the delta chain
            const uint64_t packed = r.varint();
            if (!r.ok()) { out.error = "The file is corrupt or truncated (input " + std::to_string(i) + ")."; return out; }
            InputEvent e;
            if (m.platformer) {
                frame += packed >> 3;
                const unsigned btn = static_cast<unsigned>((packed >> 1) & 3);
                if (btn < 1 || btn > 3) { out.error = "The file is corrupt (button id " + std::to_string(btn) + " at input " + std::to_string(i) + ")."; return out; }
                e.button = static_cast<Button>(btn);
                e.down = (packed & 1) != 0;
            } else {
                frame += packed >> 1;
                e.button = Button::Jump;
                e.down = (packed & 1) != 0;
            }
            e.tick = static_cast<int64_t>(frame);
            e.p2 = i >= p1Count;
            if (hasInputExt) {
                const uint64_t extLen = r.varint();
                if (!r.skip(extLen)) { out.error = "The file is corrupt (input extension overruns the file)."; return out; }
            }
            m.inputs.push_back(e);
        }
        if (!r.atEnd()) out.warnings.push_back("Ignored " + std::to_string(r.remaining()) + " trailing bytes after the last input.");

        finalizeMacro(out);
        return out;
    }
};

// ------------------------------------------------------------------------------------------------
// GDR1  (msgpack ".gdr", or ".gdr.json"). Schema from maxnut/GDReplayFormat branch gdr1:
//   author, description, duration, gameVersion, version, framerate, seed, coins, ldm,
//   bot{name,version}, level{id,name}, inputs[{frame, btn, 2p, down}]
// ------------------------------------------------------------------------------------------------

// Minimal MessagePack -> matjson decoder (everything nlohmann::json::to_msgpack can emit).
class MsgPack {
public:
    explicit MsgPack(std::span<const uint8_t> d) : m_r(d) {}
    bool decode(matjson::Value& out, std::string& err) {
        out = value(0);
        if (!m_r.ok() || !m_err.empty()) { err = m_err.empty() ? "truncated msgpack data" : m_err; return false; }
        return true;
    }

private:
    matjson::Value value(int depth) {
        if (depth > 24) { m_err = "msgpack nesting too deep"; return {}; }
        const uint8_t t = m_r.u8();
        if (!m_r.ok()) return {};
        if (t <= 0x7f) return matjson::Value(static_cast<std::intmax_t>(t));
        if (t >= 0xe0) return matjson::Value(static_cast<std::intmax_t>(static_cast<int8_t>(t)));
        if ((t & 0xe0) == 0xa0) return string(t & 0x1f);
        if ((t & 0xf0) == 0x90) return array(t & 0x0f, depth);
        if ((t & 0xf0) == 0x80) return map(t & 0x0f, depth);
        switch (t) {
            case 0xc0: return matjson::Value(nullptr);
            case 0xc2: return matjson::Value(false);
            case 0xc3: return matjson::Value(true);
            case 0xca: { uint32_t b = static_cast<uint32_t>(m_r.be(4)); float f; std::memcpy(&f, &b, 4); return matjson::Value(static_cast<double>(f)); }
            case 0xcb: { uint64_t b = m_r.be(8); double d; std::memcpy(&d, &b, 8); return matjson::Value(d); }
            case 0xcc: return matjson::Value(static_cast<std::intmax_t>(m_r.be(1)));
            case 0xcd: return matjson::Value(static_cast<std::intmax_t>(m_r.be(2)));
            case 0xce: return matjson::Value(static_cast<std::intmax_t>(m_r.be(4)));
            case 0xcf: return matjson::Value(static_cast<double>(m_r.be(8)));
            case 0xd0: return matjson::Value(static_cast<std::intmax_t>(static_cast<int8_t>(m_r.be(1))));
            case 0xd1: return matjson::Value(static_cast<std::intmax_t>(static_cast<int16_t>(m_r.be(2))));
            case 0xd2: return matjson::Value(static_cast<std::intmax_t>(static_cast<int32_t>(m_r.be(4))));
            case 0xd3: return matjson::Value(static_cast<std::intmax_t>(static_cast<int64_t>(m_r.be(8))));
            case 0xd9: case 0xc4: return string(m_r.be(1));
            case 0xda: case 0xc5: return string(m_r.be(2));
            case 0xdb: case 0xc6: return string(m_r.be(4));
            case 0xdc: return array(m_r.be(2), depth);
            case 0xdd: return array(m_r.be(4), depth);
            case 0xde: return map(m_r.be(2), depth);
            case 0xdf: return map(m_r.be(4), depth);
            default: m_err = "unsupported msgpack type byte"; return {};
        }
    }
    matjson::Value string(uint64_t n) {
        if (n > m_r.remaining()) { m_err = "msgpack string overruns the file"; return {}; }
        std::string s(static_cast<size_t>(n), '\0');
        if (n) m_r.bytes(reinterpret_cast<uint8_t*>(s.data()), static_cast<size_t>(n));
        return matjson::Value(std::move(s));
    }
    matjson::Value array(uint64_t n, int depth) {
        if (n > m_r.remaining()) { m_err = "msgpack array size is impossible"; return {}; }
        auto a = matjson::Value::array();
        for (uint64_t i = 0; i < n && m_err.empty() && m_r.ok(); ++i) a.push(value(depth + 1));
        return a;
    }
    matjson::Value map(uint64_t n, int depth) {
        if (n > m_r.remaining()) { m_err = "msgpack map size is impossible"; return {}; }
        auto o = matjson::Value::object();
        for (uint64_t i = 0; i < n && m_err.empty() && m_r.ok(); ++i) {
            auto k = value(depth + 1);
            auto v = value(depth + 1);
            auto ks = k.asString();
            if (!ks) { m_err = "msgpack map key is not a string"; return {}; }
            o.set(ks.unwrap(), std::move(v));
        }
        return o;
    }
    Reader m_r;
    std::string m_err;
};

double numOr(matjson::Value const& v, char const* key, double def) {
    auto const& x = v[key];
    if (x.isNull()) return def;
    auto d = x.asDouble();
    return d ? d.unwrap() : def;
}
std::string strOr(matjson::Value const& v, char const* key) {
    auto s = v[key].asString();
    return s ? s.unwrap() : std::string();
}
bool boolOr(matjson::Value const& v, char const* key, bool def) {
    auto b = v[key].asBool();
    return b ? b.unwrap() : def;
}

ParseResult fromGdr1Json(matjson::Value const& root, std::string format) {
    ParseResult out;
    Macro& m = out.macro;
    m.format = std::move(format);
    if (!root.isObject() || !root.contains("inputs") || !root["inputs"].isArray()) {
        out.error = "Not a GDR1 macro: the top level has no \"inputs\" array.";
        return out;
    }
    m.sourceTps = numOr(root, "framerate", 240.0);
    if (!(m.sourceTps > 0.0 && m.sourceTps < 100000.0)) { out.error = "The macro has an invalid framerate."; return out; }
    m.botName = strOr(root["bot"], "name");
    m.levelId = static_cast<uint32_t>(numOr(root["level"], "id", 0));
    m.levelName = strOr(root["level"], "name");
    m.platformer = false;

    auto const& arr = root["inputs"].asArray().unwrap();
    if (arr.size() > kMaxInputs) { out.error = "The macro has an impossible number of inputs."; return out; }
    m.inputs.reserve(arr.size());
    for (size_t i = 0; i < arr.size(); ++i) {
        auto const& in = arr[i];
        if (!in.isObject() || !in.contains("frame")) { out.error = "Input " + std::to_string(i) + " has no frame."; return out; }
        InputEvent e;
        const double frame = numOr(in, "frame", -1);
        if (frame < 0) { out.error = "Input " + std::to_string(i) + " has a negative frame."; return out; }
        e.tick = static_cast<int64_t>(frame);
        const int btn = static_cast<int>(numOr(in, "btn", 1));
        if (btn < 1 || btn > 3) { out.error = "Input " + std::to_string(i) + " has an invalid button id."; return out; }
        e.button = static_cast<Button>(btn);
        if (btn != 1) m.platformer = true;
        e.p2 = boolOr(in, "2p", false);
        e.down = boolOr(in, "down", false);
        m.inputs.push_back(e);
    }
    finalizeMacro(out);
    return out;
}

class Gdr1MsgPackParser final : public IMacroParser {
public:
    char const* name() const override { return "GDR1 (msgpack)"; }
    std::vector<std::string> extensions() const override { return {".gdr"}; }
    bool sniff(std::span<const uint8_t> d) const override {
        if (d.empty()) return false;
        const uint8_t t = d[0];
        return (t & 0xf0) == 0x80 || t == 0xde || t == 0xdf;   // top level must be a map
    }
    ParseResult parse(std::span<const uint8_t> data) const override {
        ParseResult out;
        matjson::Value v;
        std::string err;
        if (!MsgPack(data).decode(v, err)) { out.error = "The file is corrupt (" + err + ")."; return out; }
        return fromGdr1Json(v, "GDR1 (msgpack)");
    }
};

class Gdr1JsonParser final : public IMacroParser {
public:
    char const* name() const override { return "GDR1 (json)"; }
    std::vector<std::string> extensions() const override { return {".json"}; }
    bool sniff(std::span<const uint8_t> d) const override {
        bool obj = false;
        for (uint8_t c : d) {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
            obj = c == '{';
            break;
        }
        if (!obj) return false;
        // GDR1 always has an "inputs" list; other JSON macros (Mega Hack's "events", Windowpane's own file) are not GDR1
        const std::string_view all(reinterpret_cast<char const*>(d.data()), d.size());
        return all.find("\"inputs\"") != std::string_view::npos && all.find("windowpane-macro") == std::string_view::npos;
    }
    ParseResult parse(std::span<const uint8_t> data) const override {
        ParseResult out;
        auto v = matjson::Value::parse(std::string_view(reinterpret_cast<char const*>(data.data()), data.size()));
        if (!v) { out.error = "The file is not valid JSON."; return out; }
        return fromGdr1Json(v.unwrap(), "GDR1 (json)");
    }
};


// ------------------------------------------------------------------------------------------------
// Mega Hack replay as JSON (.mhr.json): { "meta": { "fps": 240, ... }, "events": [ { "frame": N, "down": bool,
// "p2": bool, "btn": 1, ... } ] }. This is read leniently (the per-event position / angle fields are ignored, the
// player-2 flag may be called p2 / 2p / player2, the button defaults to jump). Anything that does not fit is reported
// as an error instead of being guessed. Mega Hack's newer replays (2.2) use the GDR format and go through the GDR readers.
// ------------------------------------------------------------------------------------------------
class MegaHackJsonParser final : public IMacroParser {
public:
    char const* name() const override { return "Mega Hack JSON"; }
    std::vector<std::string> extensions() const override { return {".json"}; }
    bool sniff(std::span<const uint8_t> d) const override {
        const std::string_view all(reinterpret_cast<char const*>(d.data()), d.size());
        size_t i = 0;
        while (i < all.size() && (all[i] == ' ' || all[i] == '\t' || all[i] == '\r' || all[i] == '\n')) ++i;
        if (i >= all.size() || all[i] != '{') return false;
        return all.find("\"events\"") != std::string_view::npos && all.find("windowpane-macro") == std::string_view::npos &&
               all.find("\"inputs\"") == std::string_view::npos;
    }
    ParseResult parse(std::span<const uint8_t> data) const override {
        ParseResult out;
        Macro& m = out.macro;
        m.format = "Mega Hack JSON";
        m.botName = "Mega Hack";
        auto v = matjson::Value::parse(std::string_view(reinterpret_cast<char const*>(data.data()), data.size()));
        if (!v) { out.error = "The file is not valid JSON."; return out; }
        auto const& root = v.unwrap();
        if (!root.isObject() || !root.contains("events") || !root["events"].isArray()) {
            out.error = "Not a Mega Hack replay: there is no \"events\" list.";
            return out;
        }
        double fps = numOr(root["meta"], "fps", 0.0);
        if (!(fps > 0.0)) fps = numOr(root, "fps", 240.0);
        if (!(fps > 0.0 && fps < 100000.0)) { out.error = "The macro has an invalid framerate."; return out; }
        m.sourceTps = fps;
        auto const& arr = root["events"].asArray().unwrap();
        if (arr.size() > kMaxInputs) { out.error = "The macro has an impossible number of inputs."; return out; }
        m.inputs.reserve(arr.size());
        for (size_t i = 0; i < arr.size(); ++i) {
            auto const& in = arr[i];
            if (!in.isObject() || !in.contains("frame")) { out.error = "Event " + std::to_string(i) + " has no frame."; return out; }
            const double frame = numOr(in, "frame", -1);
            if (frame < 0) { out.error = "Event " + std::to_string(i) + " has a negative frame."; return out; }
            if (!in.contains("down")) { out.error = "Event " + std::to_string(i) + " does not say whether it is a press or a release."; return out; }
            InputEvent e;
            e.tick = static_cast<int64_t>(frame);
            e.down = boolOr(in, "down", false);
            e.p2 = boolOr(in, "p2", boolOr(in, "2p", boolOr(in, "player2", false)));
            int btn = static_cast<int>(numOr(in, "btn", numOr(in, "button", 1)));
            if (btn < 1 || btn > 3) { out.error = "Event " + std::to_string(i) + " has an invalid button id."; return out; }
            e.button = static_cast<Button>(btn);
            if (btn != 1) m.platformer = true;
            m.inputs.push_back(e);
        }
        out.warnings.push_back("Mega Hack's JSON replay was read with Windowpane's best-effort reader: if the macro does not survive, "
                               "re-save it from Mega Hack as .gdr (Mega Hack 2.2 can) and import that.");
        finalizeMacro(out);
        return out;
    }
};

// ------------------------------------------------------------------------------------------------
// "Other": when no reader recognises the file, look at what it contains and guess. Only structures that are unambiguous
// enough to check (a list of frames with a pressed / released state each) are accepted, and the result always says what
// was guessed so the user can judge it.
// ------------------------------------------------------------------------------------------------
namespace {

// number or numeric string
bool asNumber(matjson::Value const& v, double& out) {
    if (auto d = v.asDouble(); d) { out = d.unwrap(); return true; }
    if (auto s = v.asString(); s) {
        char* end = nullptr;
        const std::string& t = s.unwrap();
        const double x = std::strtod(t.c_str(), &end);
        if (end && end != t.c_str() && *end == '\0') { out = x; return true; }
    }
    return false;
}

// pressed / released from a bool, 0 / 1, or a word
bool asDown(matjson::Value const& v, bool& out) {
    if (auto b = v.asBool(); b) { out = b.unwrap(); return true; }
    double d;
    if (v.asDouble()) { d = v.asDouble().unwrap(); if (d == 0.0 || d == 1.0) { out = d == 1.0; return true; } return false; }
    if (auto s = v.asString(); s) {
        const std::string t = lower(s.unwrap());
        if (t == "1" || t == "down" || t == "press" || t == "pressed" || t == "click" || t == "hold" || t == "push" || t == "true") { out = true; return true; }
        if (t == "0" || t == "up" || t == "release" || t == "released" || t == "false") { out = false; return true; }
    }
    return false;
}

matjson::Value const* firstKey(matjson::Value const& o, std::initializer_list<char const*> keys) {
    for (char const* k : keys) if (o.contains(k)) return &o[k];
    return nullptr;
}

// Finds the most plausible list of input objects anywhere in the first levels of a JSON document.
bool findInputList(matjson::Value const& v, int depth, std::vector<matjson::Value> const*& best, std::string& where, std::string const& path) {
    if (depth > 3) return false;
    if (v.isArray()) {
        auto const& arr = v.asArray().unwrap();
        if (arr.size() >= 2 && arr[0].isObject() && firstKey(arr[0], {"frame", "f", "tick", "frameNumber", "Frame"}) &&
            firstKey(arr[0], {"down", "pressed", "isDown", "press", "state", "hold", "click", "action", "Down", "button_down"})) {
            if (!best || arr.size() > best->size()) { best = &arr; where = path.empty() ? "the top level" : path; }
            return true;
        }
        for (auto const& x : arr) if (x.isObject() || x.isArray()) findInputList(x, depth + 1, best, where, path);
        return best != nullptr;
    }
    if (v.isObject()) {
        for (auto const& key : {"events", "inputs", "actions", "clicks", "frames", "macro", "replay", "data", "input"}) {
            if (v.contains(key)) findInputList(v[key], depth + 1, best, where, std::string("\"") + key + "\"");
        }
        return best != nullptr;
    }
    return false;
}

ParseResult guessJson(std::string_view text) {
    ParseResult out;
    auto v = matjson::Value::parse(text);
    if (!v) { out.error = "not JSON"; return out; }
    auto const& root = v.unwrap();
    std::vector<matjson::Value> const* list = nullptr;
    std::string where;
    findInputList(root, 0, list, where, "");
    if (!list) { out.error = "no list of inputs with a frame and a pressed / released state was found in the JSON"; return out; }

    Macro& m = out.macro;
    m.format = "Guessed: JSON";
    double fps = 0.0;
    for (matjson::Value const* o : {&root, &root["meta"], &root["info"], &root["header"], &root["metadata"]}) {
        if (!o->isObject()) continue;
        if (auto const* f = firstKey(*o, {"fps", "framerate", "FPS", "tps", "TPS", "frameRate"}); f && asNumber(*f, fps) && fps > 0.0) break;
        fps = 0.0;
    }
    bool fpsFound = fps > 0.0;
    if (!fpsFound) fps = 240.0;
    if (!(fps > 0.0 && fps < 100000.0)) { out.error = "the framerate in the file is impossible"; return out; }
    m.sourceTps = fps;
    for (matjson::Value const* o : {&root, &root["meta"], &root["bot"], &root["info"]}) {
        if (!o->isObject()) continue;
        if (auto const* b = firstKey(*o, {"bot", "botName", "name", "author_bot"}); b && b->asString()) { m.botName = b->asString().unwrap(); break; }
    }

    size_t bad = 0;
    double lastFrame = -1.0;
    bool sorted = true;
    for (auto const& in : *list) {
        if (!in.isObject()) { ++bad; continue; }
        double frame = 0;
        bool down = false;
        auto const* fk = firstKey(in, {"frame", "f", "tick", "frameNumber", "Frame"});
        auto const* dk = firstKey(in, {"down", "pressed", "isDown", "press", "state", "hold", "click", "action", "Down", "button_down"});
        if (!fk || !dk || !asNumber(*fk, frame) || frame < 0 || !asDown(*dk, down)) { ++bad; continue; }
        InputEvent e;
        e.tick = static_cast<int64_t>(frame);
        e.down = down;
        if (auto const* pk = firstKey(in, {"p2", "2p", "player2", "isP2", "secondPlayer"}); pk) { bool b; if (asDown(*pk, b)) e.p2 = b; }
        if (auto const* bk = firstKey(in, {"btn", "button"}); bk) {
            double b;
            if (asNumber(*bk, b) && b >= 1 && b <= 3) { e.button = static_cast<Button>(static_cast<int>(b)); if (b != 1) m.platformer = true; }
        }
        if (frame < lastFrame) sorted = false;
        lastFrame = frame;
        m.inputs.push_back(e);
    }
    if (m.inputs.size() < 2 || bad * 10 > list->size()) { out.error = "the list of inputs in the JSON is not consistent enough to trust"; m.inputs.clear(); return out; }
    out.warnings.push_back("Guessed layout: JSON, inputs found in " + where + (fpsFound ? ", framerate " + std::to_string(static_cast<int>(std::lround(fps))) : ", no framerate in the file so 240 was assumed") +
                           (sorted ? "" : ", frames were not in order (they were sorted)") + (bad ? ", " + std::to_string(bad) + " unreadable entries skipped" : "") + ".");
    finalizeMacro(out);
    return out;
}

// Plain text: one input per line, a frame number and a pressed / released value, separated by spaces, tabs, | , or ;
ParseResult guessText(std::string_view text) {
    ParseResult out;
    Macro& m = out.macro;
    m.format = "Guessed: text";
    struct Row { std::vector<std::string> t; };
    std::vector<Row> rows;
    double fps = 0.0;
    bool fpsFound = false;
    size_t pos = 0, lines = 0;
    while (pos <= text.size() && lines < 6'000'000) {
        size_t nl = text.find('\n', pos);
        std::string_view line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        ++lines;
        Row row;
        std::string cur;
        for (char c : line) {
            if (c == ' ' || c == '\t' || c == '|' || c == ',' || c == ';' || c == '\r') { if (!cur.empty()) { row.t.push_back(cur); cur.clear(); } }
            else cur += c;
        }
        if (!cur.empty()) row.t.push_back(cur);
        if (row.t.empty() || row.t[0][0] == '#' || row.t[0].rfind("//", 0) == 0) continue;
        rows.push_back(std::move(row));
    }
    if (rows.size() < 3) { out.error = "too little text to be a macro"; return out; }
    // a first line with a single number is the framerate
    if (rows[0].t.size() == 1) {
        char* end = nullptr;
        const double f = std::strtod(rows[0].t[0].c_str(), &end);
        if (end && *end == '\0' && f >= 10.0 && f <= 100000.0) { fps = f; fpsFound = true; rows.erase(rows.begin()); }
    }
    auto num = [](std::string const& t, double& d) { char* end = nullptr; d = std::strtod(t.c_str(), &end); return end && end != t.c_str() && *end == '\0'; };
    auto word = [](std::string const& t, bool& b) {
        const std::string l = lower(t);
        if (l == "1" || l == "down" || l == "press" || l == "pressed" || l == "click" || l == "hold" || l == "push") { b = true; return true; }
        if (l == "0" || l == "up" || l == "release" || l == "released") { b = false; return true; }
        return false;
    };
    // the state column: the first column after the frame whose value is 0 / 1 or a press / release word in (nearly) every row
    size_t cols = 0;
    for (auto const& r : rows) cols = std::max(cols, r.t.size());
    int stateCol = -1;
    for (size_t c = 1; c < cols && stateCol < 0; ++c) {
        size_t good = 0;
        for (auto const& r : rows) { bool b; if (c < r.t.size() && word(r.t[c], b)) ++good; }
        if (good * 10 >= rows.size() * 9) stateCol = static_cast<int>(c);
    }
    if (stateCol < 0) { out.error = "no column that looks like pressed / released was found in the text"; return out; }
    size_t bad = 0;
    double last = -1.0;
    bool sorted = true;
    for (auto const& r : rows) {
        double f;
        bool b;
        if (r.t.empty() || !num(r.t[0], f) || f < 0 || static_cast<size_t>(stateCol) >= r.t.size() || !word(r.t[stateCol], b)) { ++bad; continue; }
        InputEvent e;
        e.tick = static_cast<int64_t>(f);
        e.down = b;
        if (f < last) sorted = false;
        last = f;
        m.inputs.push_back(e);
    }
    if (m.inputs.size() < 2 || bad * 10 > rows.size()) { out.error = "the text is not consistent enough to trust"; m.inputs.clear(); return out; }
    m.sourceTps = fpsFound ? fps : 240.0;
    out.warnings.push_back("Guessed layout: text, frame in the first column and pressed / released in column " + std::to_string(stateCol + 1) +
                           (fpsFound ? ", framerate " + std::to_string(static_cast<int>(std::lround(fps))) : ", no framerate in the file so 240 was assumed") +
                           (sorted ? "" : ", frames were not in order (they were sorted)") + (bad ? ", " + std::to_string(bad) + " unreadable lines skipped" : "") + ".");
    finalizeMacro(out);
    return out;
}

// What the file's own header says about the bot, mapped to an import type (used for the timing order and as information).
MacroType typeFromBot(std::string const& botName) {
    const std::string b = lower(botName);
    if (b.find("xdbot") != std::string::npos) return MacroType::XdBot;
    if (b.find("zbot") != std::string::npos) return MacroType::ZBot;
    if (b.find("eclipse") != std::string::npos) return MacroType::Eclipse;
    if (b.find("mega") != std::string::npos || b.find("hack") != std::string::npos) return MacroType::MegaHack;
    return MacroType::Auto;
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// Windowpane macro (.wpmacro): the macro plus, optionally, its analysis, written by "Save macro".
// ------------------------------------------------------------------------------------------------
class WindowpaneParser final : public IMacroParser {
public:
    char const* name() const override { return "Windowpane macro"; }
    std::vector<std::string> extensions() const override { return {".wpmacro"}; }
    bool sniff(std::span<const uint8_t> d) const override {
        // The marker is not necessarily near the start: keys are not guaranteed to be written in a fixed order and a
        // saved analysis can be large, so look through the whole file.
        const std::string_view all(reinterpret_cast<char const*>(d.data()), d.size());
        return all.find("windowpane-macro") != std::string_view::npos;
    }
    ParseResult parse(std::span<const uint8_t> data) const override {
        ParseResult out;
        Macro& m = out.macro;
        m.format = "Windowpane macro";
        auto v = matjson::Value::parse(std::string_view(reinterpret_cast<char const*>(data.data()), data.size()));
        if (!v) { out.error = "The file is not valid JSON."; return out; }
        auto const& root = v.unwrap();
        if (!root.isObject() || strOr(root, "format") != "windowpane-macro" || !root.contains("inputs") || !root["inputs"].isArray()) {
            out.error = "Not a Windowpane macro file.";
            return out;
        }
        m.sourceTps = 240.0;
        m.botName = strOr(root, "botName");
        m.levelId = static_cast<uint32_t>(numOr(root, "levelId", 0));
        m.levelName = strOr(root, "levelName");
        m.platformer = boolOr(root, "platformer", false);
        auto const& arr = root["inputs"].asArray().unwrap();
        if (arr.size() > kMaxInputs) { out.error = "The macro has an impossible number of inputs."; return out; }
        for (size_t i = 0; i < arr.size(); ++i) {
            auto const& in = arr[i];
            const double tick = numOr(in, "tick", -1);
            if (tick < 0) { out.error = "Input " + std::to_string(i) + " has a negative tick."; return out; }
            InputEvent e;
            e.tick = static_cast<int64_t>(tick);
            e.sub = numOr(in, "sub", 0.0);
            const int btn = static_cast<int>(numOr(in, "button", 1));
            if (btn < 1 || btn > 3) { out.error = "Input " + std::to_string(i) + " has an invalid button id."; return out; }
            e.button = static_cast<Button>(btn);
            e.p2 = boolOr(in, "p2", false);
            e.down = boolOr(in, "down", false);
            e.raw = static_cast<int64_t>(numOr(in, "raw", tick));   // the frame number as it was in the original file
            if (e.sub > 0.0) m.hasSubTick = true;
            m.inputs.push_back(e);
        }
        const size_t savedCount = m.inputs.size();
        out.keepRaw = true;   // keep the saved frame numbers / "imported" state (see finalizeMacro)
        finalizeMacro(out);
        if (!out.ok()) return out;
        // Restore what finalizeMacro would have derived from the format name. A macro that came from another bot
        // (GDR) must stay "imported" after Save macro -> Load, otherwise the automatic timing fix is skipped and the
        // macro dies in the level even though the original file replays fine.
        m.imported = boolOr(root, "imported", false);
        m.timingScale = numOr(root, "timingScale", 1.0);
        m.timingOffset = numOr(root, "timingOffset", 0.0);

        if (root.contains("analysis") && root["analysis"].isObject()) {
            auto const& a = root["analysis"];
            if (a.contains("inputs") && a["inputs"].isArray() && a["inputs"].asArray().unwrap().size() == savedCount && m.inputs.size() == savedCount) {
                auto res = std::make_shared<AnalysisResult>();
                res->macroName = m.name;
                res->levelId = m.levelId;
                res->levelName = m.levelName;
                res->cbf = boolOr(a, "cbf", false);
                res->cbfSupported = boolOr(a, "cbfSupported", false);
                res->maxShift = static_cast<int>(numOr(a, "maxShift", 0));
                res->totalTrials = static_cast<int64_t>(numOr(a, "totalTrials", 0));
                res->totalTicks = static_cast<int64_t>(numOr(a, "totalTicks", 0));
                res->elapsedSec = numOr(a, "elapsedSeconds", 0.0);
                res->sigmaPerSec = numOr(a, "sigmaPerSecond", 0.0);
                res->sigmaHours = numOr(a, "sigmaAssumedHours", 24.0);
                res->complete = true;
                if (a.contains("warnings") && a["warnings"].isArray())
                    for (auto const& w : a["warnings"].asArray().unwrap()) { auto s = w.asString(); if (s) res->warnings.push_back(s.unwrap()); }
                auto const& ins = a["inputs"].asArray().unwrap();
                res->inputs.resize(ins.size());
                for (size_t i = 0; i < ins.size(); ++i) {
                    auto const& q = ins[i];
                    InputResult& r = res->inputs[i];
                    r.index = static_cast<int>(i);
                    const int st = static_cast<int>(numOr(q, "status", 0));
                    r.status = (st >= 0 && st <= static_cast<int>(InputStatus::NotApplicable)) ? static_cast<InputStatus>(st) : InputStatus::Pending;
                    r.window = numOr(q, "window", 0.0);
                    r.earlier = numOr(q, "earlier", 0.0);
                    r.later = numOr(q, "later", 0.0);
                    r.trials = static_cast<int>(numOr(q, "trials", 0));
                    r.x = static_cast<float>(numOr(q, "x", 0.0));
                    r.y = static_cast<float>(numOr(q, "y", 0.0));
                    r.percent = static_cast<float>(numOr(q, "percent", 0.0));
                    r.timeSec = numOr(q, "timeSec", 0.0);
                    r.note = strOr(q, "note");
                    r.trace = strOr(q, "trace");
                    r.edited = boolOr(q, "edited", false);
                    r.analyzedWindow = numOr(q, "analyzedWindow", r.window);
                }
                out.analysis = res;
            } else {
                out.warnings.push_back("The saved analysis does not match the macro's inputs and was ignored.");
            }
        }
        return out;
    }
};

}  // namespace

void registerParser(std::unique_ptr<IMacroParser> p) { registry().push_back(std::move(p)); }

void registerBuiltInParsers() {
    if (!registry().empty()) return;
    registerParser(std::make_unique<WindowpaneParser>());   // before the generic JSON parser
    registerParser(std::make_unique<Gdr2Parser>());
    registerParser(std::make_unique<Gdr1MsgPackParser>());
    registerParser(std::make_unique<MegaHackJsonParser>());   // before the generic GDR1 JSON parser (which accepts any object)
    registerParser(std::make_unique<Gdr1JsonParser>());
}

std::vector<std::string> supportedExtensions() {
    std::vector<std::string> out;
    for (auto& p : registry())
        for (auto& e : p->extensions())
            if (std::find(out.begin(), out.end(), e) == out.end()) out.push_back(e);
    return out;
}

std::shared_ptr<Macro const> rescaleMacro(Macro const& m, double scale, double offset, bool half) {
    auto out = std::make_shared<Macro>(m);
    const double base = 1.0;   // ticks stay in the macro's own tick rate
    for (auto& e : out->inputs) {
        double pos = static_cast<double>(e.raw) * base * scale + offset;
        if (pos < 0.0) pos = 0.0;
        if (half) {
            const double q = std::round(pos * 2.0) / 2.0;   // nearest half tick
            e.tick = static_cast<int64_t>(std::floor(q + 1e-9));
            e.sub = q - static_cast<double>(e.tick) >= 0.25 ? 0.5 : 0.0;
        } else {
            e.tick = static_cast<int64_t>(std::llround(pos));
            e.sub = 0.0;
        }
    }
    std::stable_sort(out->inputs.begin(), out->inputs.end(), [](InputEvent const& a, InputEvent const& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        return a.sub < b.sub;
    });
    out->lastTick = out->inputs.empty() ? 0 : out->inputs.back().tick;
    out->hasSubTick = std::any_of(out->inputs.begin(), out->inputs.end(), [](InputEvent const& e) { return e.sub != 0.0; });
    out->timingScale = scale;
    out->timingOffset = offset;
    return out;
}

void finalizeMacro(ParseResult& r) {
    Macro& m = r.macro;

    if (!r.keepRaw) {
        for (auto& e : m.inputs) e.raw = e.tick;   // remember the frame numbers as written in the file
        m.imported = m.format.rfind("GDR", 0) == 0 || m.format.rfind("Mega Hack", 0) == 0 || m.format.rfind("Guessed", 0) == 0;
    }

    // Ticks stay in the macro's own tick rate (sourceTps). It is analysed at exactly that rate: the FPS option on the main page
    // has to be set to it first (checked when the analysis starts), because physics at another rate does not reproduce.

    // --- order -----------------------------------------------------------------------------------
    std::stable_sort(m.inputs.begin(), m.inputs.end(), [](InputEvent const& a, InputEvent const& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        return false;   // keep file order for same-tick events (a release then a press stays that way)
    });

    // --- drop events that cannot do anything: a press while already held, a release while up ------
    // Each (player, button) is a little state machine that starts "up". Redundant events would give
    // meaningless windows and shifting them would not change the run.
    std::map<std::pair<bool, int>, bool> held;
    std::vector<InputEvent> clean;
    clean.reserve(m.inputs.size());
    size_t dropped = 0;
    for (auto const& e : m.inputs) {
        bool& h = held[{e.p2, static_cast<int>(e.button)}];
        if (e.down == h) { ++dropped; continue; }
        h = e.down;
        clean.push_back(e);
    }
    if (dropped) r.warnings.push_back(std::to_string(dropped) + " redundant press/release events were ignored.");
    m.inputs = std::move(clean);

    m.lastTick = m.inputs.empty() ? 0 : m.inputs.back().tick;
    m.hasSubTick = std::any_of(m.inputs.begin(), m.inputs.end(), [](InputEvent const& e) { return e.sub != 0.0; });
}

char const* macroTypeName(MacroType t) {
    switch (t) {
        case MacroType::XdBot: return "xdBot";
        case MacroType::ZBot: return "zBot";
        case MacroType::Eclipse: return "Eclipse";
        case MacroType::Windowpane: return "Windowpane Macro";
        case MacroType::MegaHack: return "Mega Hack";
        default: return "Other";
    }
}

char const* macroTypeSummary(MacroType t) {
    switch (t) {
        case MacroType::XdBot: return ".gdr (current xdBot). Old .xd files must be converted first.";
        case MacroType::ZBot: return ".gdr (current zBot). Old .zbf files must be converted first.";
        case MacroType::Eclipse: return ".gdr2 / .gdr (Eclipse Menu's replay format).";
        case MacroType::Windowpane: return ".wpmacro, written by Save macro (can carry its analysis).";
        case MacroType::MegaHack: return ".gdr (Mega Hack 2.2) or .mhr.json. Binary .mhr must be converted.";
        default: return "Not listed? Windowpane tries every reader, then guesses JSON / text layouts.";
    }
}

std::vector<std::string> macroTypeExtensions(MacroType t) {
    switch (t) {
        case MacroType::XdBot: return {".gdr", ".gdr2", ".json", ".xd"};
        case MacroType::ZBot: return {".gdr", ".gdr2", ".json", ".zbf"};
        case MacroType::Eclipse: return {".gdr2", ".gdr"};
        case MacroType::Windowpane: return {".wpmacro"};
        case MacroType::MegaHack: return {".gdr", ".gdr2", ".json", ".mhr"};
        default: {
            registerBuiltInParsers();
            return {".*"};   // any file: the content decides
        }
    }
}

namespace {
constexpr char kConverterHelp[] = "ZCB 3's web converter (zeozeozeo.github.io/zcb3) converts any of these formats to .gdr";

// Files this reader cannot parse at all: say what to do instead of calling them "unrecognised".
std::string unreadableFormatHelp(std::string const& ext) {
    if (ext == ".xd")
        return std::string("This is an old xdBot macro (.xd), which Windowpane cannot read. Open it in xdBot and save it as .gdr, or convert it: ") + kConverterHelp + ".";
    if (ext == ".cml")
        return std::string("This is a compressed xdBot macro (.cml), which Windowpane cannot read. Save it as .gdr from xdBot, or convert it: ") + kConverterHelp + ".";
    if (ext == ".zbf")
        return std::string("This is an old zBot Frame macro (.zbf), which Windowpane cannot read. Re-save it as .gdr from zBot, or convert it: ") + kConverterHelp + ".";
    if (ext == ".mhr")
        return std::string("This is a binary Mega Hack replay (.mhr), which Windowpane cannot read. Save it as .gdr from Mega Hack 2.2 (or as .mhr.json), or convert it: ") + kConverterHelp + ".";
    return {};
}

// Which readers belong to which type. Names are IMacroParser::name().
bool parserAllowed(MacroType t, IMacroParser const& p) {
    const std::string n = p.name();
    const bool gdr = n == "GDR2" || n == "GDR1 (msgpack)" || n == "GDR1 (json)";
    switch (t) {
        case MacroType::XdBot:
        case MacroType::ZBot:
        case MacroType::Eclipse: return gdr;
        case MacroType::Windowpane: return n == "Windowpane macro";
        case MacroType::MegaHack: return gdr || n == "Mega Hack JSON";
        default: return true;
    }
}

// A GDR header names the bot that wrote it. A different name is only worth a warning (bots are forked and renamed).
void checkBotName(MacroType t, ParseResult& r) {
    char const* want = nullptr;
    char const* alt = nullptr;
    switch (t) {
        case MacroType::XdBot: want = "xdbot"; break;
        case MacroType::ZBot: want = "zbot"; break;
        case MacroType::Eclipse: want = "eclipse"; break;
        case MacroType::MegaHack: want = "mega"; alt = "hack"; break;
        default: return;
    }
    const std::string bot = lower(r.macro.botName);
    if (!bot.empty() && bot.find(want) == std::string::npos && !(alt && bot.find(alt) != std::string::npos))
        r.warnings.push_back("The file says it was recorded with \"" + r.macro.botName + "\", not " + macroTypeName(t) +
                             ". It was read as " + macroTypeName(t) + " anyway; if it does not replay, pick the right type.");
    if (t == MacroType::Eclipse && r.macro.format.rfind("GDR1", 0) == 0)
        r.warnings.push_back("Eclipse writes GDR2 files; this one is an older GDR1 file.");
}

std::string wrongTypeError(MacroType t) {
    switch (t) {
        case MacroType::Windowpane: return "This is not a Windowpane macro. Windowpane macros are .wpmacro files written by Save macro; for a macro from a bot pick that bot instead.";
        case MacroType::XdBot: return "Windowpane could not read this as an xdBot macro. Current xdBot saves .gdr files; for other bots pick their type.";
        case MacroType::ZBot: return "Windowpane could not read this as a zBot macro. zBot saves .gdr files (old .zbf files must be converted first).";
        case MacroType::Eclipse: return "Windowpane could not read this as an Eclipse macro. Eclipse saves .gdr2 (or .gdr) files.";
        case MacroType::MegaHack: return "Windowpane could not read this as a Mega Hack macro. It reads .gdr (Mega Hack 2.2) and .mhr.json files; binary .mhr must be converted first.";
        default: return "Unrecognised macro format. Supported: .gdr / .gdr2 (GDR1 and GDR2), .wpmacro, .mhr.json and .gdr.json.";
    }
}
}  // namespace

namespace {
ParseResult finishGuess(ParseResult g, std::filesystem::path const& path) {
    g.macro.path = path.string();
    g.macro.name = path.stem().string();
    g.macro.type = typeFromBot(g.macro.botName);
    g.warnings.insert(g.warnings.begin(), "Other: none of the known formats matched, so the file was guessed (" + g.macro.format + "). Check that the macro behaves as expected.");
    return g;
}
}  // namespace

ParseResult loadMacroFile(std::filesystem::path const& path, MacroType type) {
    registerBuiltInParsers();
    ParseResult out;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) { out.error = "File not found: " + path.string(); return out; }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0) { out.error = "The file is empty or unreadable."; return out; }
    if (size > kMaxFileBytes) { out.error = "The file is larger than 64 MB; refusing to load it."; return out; }

    const std::string ext = lower(path.extension().string());
    // Formats that exist but cannot be read here: tell the user what to do, whichever type was picked.
    if (auto help = unreadableFormatHelp(ext); !help.empty()) { out.error = help; return out; }

    std::ifstream f(path, std::ios::binary);
    if (!f) { out.error = "Could not open the file."; return out; }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) { out.error = "Could not read the whole file."; return out; }

    // prefer parsers that claim the extension, then fall back to any allowed parser whose sniff() accepts
    std::vector<IMacroParser*> order;
    for (auto& p : registry()) {
        if (!parserAllowed(type, *p)) continue;
        auto e = p->extensions();
        if (std::find(e.begin(), e.end(), ext) != e.end()) order.push_back(p.get());
    }
    for (auto& p : registry())
        if (parserAllowed(type, *p) && std::find(order.begin(), order.end(), p.get()) == order.end()) order.push_back(p.get());

    for (auto* p : order) {
        if (!p->sniff(bytes)) continue;
        out = p->parse(bytes);
        if (out.ok()) {
            out.macro.path = path.string();
            out.macro.name = path.stem().string();
            if (out.macro.name.size() > 4 && lower(out.macro.name.substr(out.macro.name.size() - 4)) == ".gdr")
                out.macro.name.resize(out.macro.name.size() - 4);   // "x.gdr.json" -> "x"
            if (out.macro.name.size() > 4 && lower(out.macro.name.substr(out.macro.name.size() - 4)) == ".mhr")
                out.macro.name.resize(out.macro.name.size() - 4);   // "x.mhr.json" -> "x"
            out.macro.type = type;
            checkBotName(type, out);
            if (type == MacroType::Auto) {
                // "Other": say what was worked out, and remember which bot the file names (it picks the timing order)
                const MacroType guessed = typeFromBot(out.macro.botName);
                out.macro.type = guessed;
                out.warnings.insert(out.warnings.begin(),
                    "Other: read as " + out.macro.format + (out.macro.botName.empty() ? std::string() : ", recorded with \"" + out.macro.botName + "\"") +
                    (guessed != MacroType::Auto ? std::string(" (treated as ") + macroTypeName(guessed) + ")" : std::string()) +
                    ", " + std::to_string(out.macro.inputs.size()) + " inputs at " + std::to_string(static_cast<int>(std::lround(out.macro.sourceTps))) + " TPS.");
            }
            return out;
        }
        if (std::find(p->extensions().begin(), p->extensions().end(), ext) != p->extensions().end() && !out.error.empty())
            return out;   // the format matched but the file is damaged: report that, not "unknown format"
    }

    // Nothing allowed accepted it. If a reader of ANOTHER type does, say so: the wrong type was probably picked.
    out = ParseResult{};
    if (type == MacroType::Auto) {
        // Nothing recognised it. Look at what is inside and guess.
        std::string why;
        size_t i = 0;
        while (i < bytes.size() && (bytes[i] == ' ' || bytes[i] == '\t' || bytes[i] == '\r' || bytes[i] == '\n')) ++i;
        const std::string_view text(reinterpret_cast<char const*>(bytes.data()), bytes.size());
        if (i < bytes.size() && (bytes[i] == '{' || bytes[i] == '[')) {
            auto g = guessJson(text);
            if (g.ok()) { return finishGuess(std::move(g), path); }
            why = "As JSON: " + g.error + ".";
        } else {
            bool binary = false;
            for (size_t k = 0; k < std::min<size_t>(bytes.size(), 4096); ++k) if (bytes[k] == 0) { binary = true; break; }
            if (!binary) {
                auto g = guessText(text);
                if (g.ok()) { return finishGuess(std::move(g), path); }
                why = "As text: " + g.error + ".";
            } else {
                why = "It is a binary file in a layout Windowpane does not know.";
            }
        }
        out.error = "Windowpane tried every reader and every layout it can guess, and none fits this file. " + why +
                    " Supported: .gdr / .gdr2 (xdBot, zBot, Eclipse, Mega Hack 2.2), .wpmacro and .mhr.json; other bots' files can be converted to .gdr with ZCB 3's web converter (zeozeozeo.github.io/zcb3).";
        return out;
    }
    if (type != MacroType::Auto) {
        for (auto& p : registry()) {
            if (parserAllowed(type, *p) || !p->sniff(bytes)) continue;
            out.error = std::string("This file looks like a ") + p->name() + " file, which does not match the type you picked (" +
                        macroTypeName(type) + "). " + wrongTypeError(type);
            return out;
        }
    }
    out.error = wrongTypeError(type);
    return out;
}

std::string validateAgainstLevel(Macro const& m, LevelInfo const& level, std::vector<std::string>& warnings) {
    if (m.inputs.empty()) return "The macro contains no inputs.";
    if (m.levelId != 0 && level.id != 0 && m.levelId != level.id) {
        return "Wrong level: the macro was recorded on level " + std::to_string(m.levelId) +
               (m.levelName.empty() ? "" : " (" + m.levelName + ")") + ", but level " + std::to_string(level.id) +
               (level.name.empty() ? "" : " (" + level.name + ")") + " is open.";
    }
    if (m.levelId == 0 && !m.levelName.empty() && !level.name.empty() && m.levelName != level.name)
        warnings.push_back("The macro says it is for \"" + m.levelName + "\" but \"" + level.name + "\" is open.");
    if (m.platformer != level.platformer) {
        return std::string("Mode mismatch: the macro is a ") + (m.platformer ? "platformer" : "classic") +
               " macro but the open level is " + (level.platformer ? "platformer" : "classic") + ".";
    }
    if (!m.platformer) {
        for (auto const& e : m.inputs)
            if (e.button != Button::Jump) return "The macro contains left/right inputs but the level is not a platformer.";
    }
    if (m.inputs.front().tick < 0) return "The macro has a negative first frame.";
    return {};
}

}  // namespace wp
