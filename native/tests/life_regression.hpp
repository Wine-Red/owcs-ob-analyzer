#pragma once
#include "core/recognizer.hpp"
#include <utility>

// Returns {checks, failures}; images are captured game frames, not templates.
std::pair<int, int> lifeRegression(owcs::Recognizer &recognizer, const QString &directory);
