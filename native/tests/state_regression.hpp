#pragma once
#include "core/recognizer.hpp"

// Inputs and expected transitions are constructed independently of State.
QJsonObject stateRegression(const owcs::Observation &base);
