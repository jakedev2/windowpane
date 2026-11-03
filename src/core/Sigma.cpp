#include "Sigma.hpp"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>

namespace wp {

/*
 * sigma/s - how precise do you have to be?
 * ----------------------------------------
 * Model the player's timing error on every input as an independent zero-mean Gaussian with standard
 * deviation sigma (seconds). An input whose window is w ticks is `W = w / 240` seconds wide, and it is
 * hit when the error lands inside the window. Treating the window as centred on the player's aim:
 *
 *     p_i(sigma) = P(|e| <= W_i / 2) = erf( W_i / (2 * sqrt(2) * sigma) )
 *
 * One attempt that is played to the end succeeds with  P_run = prod_i p_i.  An attempt that fails on
 * input k lasts only t_k (the time of that input) plus a respawn delay, so the expected length of one
 * attempt is
 *
 *     A = sum_k (t_k + R) * ( prod_{j<k} p_j ) * (1 - p_k)  +  (T_level + R) * P_run
 *
 * and the expected time to finish the level from 0% is  E(sigma) = A / P_run  (the geometric distribution
 * of "number of attempts until the first success").
 *
 * E grows monotonically with sigma (worse precision => longer grind), so we bisect for the sigma at
 * which E equals the assumed completion time (24 h by default). That sigma is the *largest* timing
 * noise a player may have and still expect to finish inside the budget. Tighter windows make it
 * smaller. The rating shown to the user is its reciprocal, 1 / sigma, in "per second", so bigger
 * means harder - which is the ordering of the sigma/s figures quoted for the reference video.
 *
 * Everything is done in log space because P_run is routinely 1e-40 or smaller on long macros.
 */

namespace {
constexpr double kRespawnSeconds = 1.0;

struct Item {
    double width;   // seconds
    double t;       // seconds from attempt start
};

double logExpectedTime(std::vector<Item> const& items, double levelSeconds, double sigma) {
    const double k = 2.0 * std::sqrt(2.0) * sigma;
    double logRun = 0.0;     // ln P(reach input k)
    double sumFail = 0.0;    // sum of (t+R) * P(die at k) with P(reach) kept as a double
    double reach = 1.0;
    for (auto const& it : items) {
        double p = std::erf(it.width / k);
        p = std::clamp(p, 1e-300, 1.0);
        sumFail += (it.t + kRespawnSeconds) * reach * (1.0 - p);
        reach *= p;
        logRun += std::log(p);
    }
    const double attempt = sumFail + (levelSeconds + kRespawnSeconds) * reach;
    // E = attempt / P_run, with P_run = exp(logRun). attempt >= (levelSeconds + R) * reach > 0 unless
    // reach underflowed; in that case fall back to the dominant term.
    const double safeAttempt = std::max(attempt, (levelSeconds + kRespawnSeconds) * 1e-300);
    return std::log(safeAttempt) - logRun;
}
}  // namespace

SigmaInfo computeSigma(Macro const& macro, AnalysisResult const& result, double completionHours) {
    SigmaInfo out;
    std::vector<Item> items;
    items.reserve(result.inputs.size());
    for (auto const& r : result.inputs) {
        if (r.status != InputStatus::Measured && r.status != InputStatus::Capped) {
            ++out.skipped;
            continue;
        }
        if (r.index < 0 || static_cast<size_t>(r.index) >= macro.inputs.size()) {
            ++out.skipped;
            continue;
        }
        const double w = std::max(1.0, r.window);
        items.push_back({w * (1.0 / tps()),
                         static_cast<double>(macro.inputs[r.index].tick) * (1.0 / tps())});
    }
    out.used = static_cast<int>(items.size());
    if (items.empty() || completionHours <= 0.0) return out;

    std::sort(items.begin(), items.end(), [](Item const& a, Item const& b) { return a.t < b.t; });
    const double levelSeconds = static_cast<double>(macro.lastTick) * (1.0 / tps());
    const double target = std::log(completionHours * 3600.0);

    double lo = 1e-6, hi = 10.0;   // seconds
    if (logExpectedTime(items, levelSeconds, hi) <= target) {
        out.sigmaSeconds = hi;     // trivially easy: even 10 s of noise finishes in time
    } else if (logExpectedTime(items, levelSeconds, lo) >= target) {
        out.sigmaSeconds = lo;     // beyond the model's resolution
    } else {
        for (int i = 0; i < 100; ++i) {
            const double mid = std::sqrt(lo * hi);   // geometric bisection: sigma spans decades
            if (logExpectedTime(items, levelSeconds, mid) > target) hi = mid; else lo = mid;
        }
        out.sigmaSeconds = std::sqrt(lo * hi);
    }
    out.perSecond = 1.0 / out.sigmaSeconds;
    out.valid = true;
    return out;
}

std::string formatSigmaRating(SigmaInfo const& s) {
    if (!s.valid) return "n/a";
    if (s.perSecond >= 100.0) return fmt::format("{:.0f}", s.perSecond);
    if (s.perSecond >= 10.0) return fmt::format("{:.1f}", s.perSecond);
    return fmt::format("{:.2f}", s.perSecond);
}

}  // namespace wp
