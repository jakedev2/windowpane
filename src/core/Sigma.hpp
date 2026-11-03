#pragma once
#include "Types.hpp"

namespace wp {

struct SigmaInfo {
    double sigmaSeconds = 0.0;   // largest tolerable timing standard deviation
    double perSecond = 0.0;      // 1 / sigmaSeconds  - the difficulty rating shown as "sigma/s"
    int used = 0;                // inputs that contributed
    int skipped = 0;             // inputs left out (baseline failure, unreliable, pending)
    bool valid = false;
};

// Difficulty rating derived from the window data. The maths is explained in Sigma.cpp.
SigmaInfo computeSigma(Macro const& macro, AnalysisResult const& result, double completionHours);

// Human readable text such as "412.7" (rating) with the sigma in ms appended by callers.
std::string formatSigmaRating(SigmaInfo const& s);

}  // namespace wp
