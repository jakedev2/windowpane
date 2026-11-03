#include <cmath>
#include "Export.hpp"

#include "../core/Cfg.hpp"

#include <Geode/Geode.hpp>
#include <fmt/format.h>
#include <fstream>
#include <map>

using namespace geode::prelude;

namespace wp {

static char const* statusName(InputStatus s) {
    switch (s) {
        case InputStatus::Pending: return "pending";
        case InputStatus::Measured: return "measured";
        case InputStatus::Capped: return "capped";
        case InputStatus::BaselineFail: return "baseline-fail";
        case InputStatus::Unreliable: return "unreliable";
        case InputStatus::Aborted: return "aborted";
        case InputStatus::NotApplicable: return "not-applicable";
    }
    return "?";
}

std::string exportJsonText(Macro const& m, AnalysisResult const& r) {
    auto root = matjson::makeObject({
        {"mod", "jake.windowpane"},
        {"macro", m.name},
        {"format", m.format},
        {"levelId", static_cast<std::intmax_t>(r.levelId)},
        {"levelName", r.levelName},
        {"tps", static_cast<int>(std::lround(m.sourceTps))},
        {"cbf", r.cbfSupported},
        {"maxShift", r.maxShift},
        {"complete", r.complete},
        {"cancelled", r.cancelled},
        {"error", r.error},
        {"totalTrials", static_cast<std::intmax_t>(r.totalTrials)},
        {"totalTicks", static_cast<std::intmax_t>(r.totalTicks)},
        {"elapsedSeconds", r.elapsedSec},
        {"sigmaPerSecond", r.sigmaPerSec},
        {"sigmaAssumedHours", r.sigmaHours},
    });
    auto warnings = matjson::Value::array();
    for (auto const& w : r.warnings) warnings.push(w);
    root["warnings"] = warnings;

    std::map<int, int> histogram;
    auto inputs = matjson::Value::array();
    for (size_t i = 0; i < r.inputs.size() && i < m.inputs.size(); ++i) {
        auto const& in = r.inputs[i];
        auto const& e = m.inputs[i];
        auto o = matjson::makeObject({
            {"index", static_cast<int>(i)},
            {"tick", static_cast<std::intmax_t>(e.tick)},
            {"sub", e.sub},
            {"type", e.down ? "press" : "release"},
            {"button", static_cast<int>(e.button)},
            {"player", e.p2 ? 2 : 1},
            {"status", statusName(in.status)},
            {"window", in.window},
            {"earlier", in.earlier},
            {"later", in.later},
            {"trials", in.trials},
            {"x", static_cast<double>(in.x)},
            {"y", static_cast<double>(in.y)},
            {"percent", static_cast<double>(in.percent)},
            {"speed", static_cast<double>(in.speed)},
            {"mode", in.mode},
        });
        if (!in.note.empty()) o["note"] = in.note;
        if (!in.trace.empty()) o["trace"] = in.trace;
        inputs.push(o);
        if (in.status == InputStatus::Measured || in.status == InputStatus::Capped) ++histogram[static_cast<int>(in.window + 1e-9)];
    }
    root["inputs"] = inputs;
    auto hist = matjson::Value::object();
    for (auto const& [w, n] : histogram) hist[std::to_string(w)] = n;
    root["histogram"] = hist;
    return root.dump(2);
}

std::string exportWpMacroText(Macro const& m, AnalysisResult const* r) {
    auto root = matjson::makeObject({
        {"format", "windowpane-macro"},
        {"version", 1},
        {"name", m.name},
        {"sourceFormat", m.format},
        {"botName", m.botName},
        {"levelId", static_cast<std::intmax_t>(m.levelId)},
        {"levelName", m.levelName},
        {"platformer", m.platformer},
        {"tps", static_cast<int>(std::lround(m.sourceTps))},
        {"imported", m.imported},
        {"timingScale", m.timingScale},
        {"timingOffset", m.timingOffset},
    });
    auto inputs = matjson::Value::array();
    for (auto const& e : m.inputs) {
        inputs.push(matjson::makeObject({
            {"tick", static_cast<std::intmax_t>(e.tick)},
            {"raw", static_cast<std::intmax_t>(e.raw)},
            {"sub", e.sub},
            {"button", static_cast<int>(e.button)},
            {"p2", e.p2},
            {"down", e.down},
        }));
    }
    root["inputs"] = inputs;
    if (r && !r->inputs.empty()) {
        auto a = matjson::makeObject({
            {"cbf", r->cbf},
            {"cbfSupported", r->cbfSupported},
            {"maxShift", r->maxShift},
            {"totalTrials", static_cast<std::intmax_t>(r->totalTrials)},
            {"totalTicks", static_cast<std::intmax_t>(r->totalTicks)},
            {"elapsedSeconds", r->elapsedSec},
            {"sigmaPerSecond", r->sigmaPerSec},
            {"sigmaAssumedHours", r->sigmaHours},
        });
        auto warnings = matjson::Value::array();
        for (auto const& w : r->warnings) warnings.push(w);
        a["warnings"] = warnings;
        auto ins = matjson::Value::array();
        for (auto const& in : r->inputs) {
            auto o = matjson::makeObject({
                {"status", static_cast<int>(in.status)},
                {"window", in.window},
                {"earlier", in.earlier},
                {"later", in.later},
                {"trials", in.trials},
                {"x", static_cast<double>(in.x)},
                {"y", static_cast<double>(in.y)},
                {"percent", static_cast<double>(in.percent)},
                {"timeSec", in.timeSec},
                {"edited", in.edited},
                {"analyzedWindow", in.analyzedWindow},
            });
            if (!in.note.empty()) o["note"] = in.note;
            if (!in.trace.empty()) o["trace"] = in.trace;
            ins.push(o);
        }
        a["inputs"] = ins;
        root["analysis"] = a;
    }
    return root.dump(2);
}

std::string exportJson(Macro const& m, AnalysisResult const& r, std::filesystem::path& outPath) {
    std::filesystem::path dir;
    std::string stem = m.name.empty() ? "macro" : m.name;
    if (!m.path.empty()) {
        std::filesystem::path p = std::filesystem::u8path(m.path);
        dir = p.parent_path();
    }
    std::error_code ec;
    if (dir.empty() || !std::filesystem::exists(dir, ec)) {
        dir = Mod::get()->getSaveDir() / "results";
        std::filesystem::create_directories(dir, ec);
    }
    outPath = dir / (stem + ".windows.json");
    std::ofstream f(outPath, std::ios::binary | std::ios::trunc);
    if (!f) return fmt::format("Could not open {} for writing.", outPath.string());
    const auto text = exportJsonText(m, r);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) return "Writing the JSON file failed.";
    return "";
}

std::string exportSummary(Macro const& m, AnalysisResult const& r) {
    auto& c = cfg();
    std::map<int, int> perBucket;
    int measured = 0;
    for (auto const& in : r.inputs) {
        if (in.status != InputStatus::Measured && in.status != InputStatus::Capped) continue;
        ++measured;
        ++perBucket[bucketIndexFor(c, in.window)];
    }
    std::string s = fmt::format("{} - {} inputs measured of {}\n", m.name, measured, r.inputs.size());
    for (size_t b = 0; b < c.buckets.size(); ++b) {
        auto it = perBucket.find(static_cast<int>(b));
        if (it == perBucket.end()) continue;
        auto const& bk = c.buckets[b];
        s += fmt::format("{}: {}\n", bk.label.empty() ? (bk.hi == kOpen ? fmt::format("{}+", bk.lo) : bk.lo == bk.hi ? std::to_string(bk.lo) : fmt::format("{}-{}", bk.lo, bk.hi)) : bk.label, it->second);
    }
    if (r.sigmaPerSec > 0) s += fmt::format("sigma/s: {:.1f}\n", r.sigmaPerSec);
    return s;
}

}  // namespace wp
