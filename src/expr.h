#pragma once

#include <functional>
#include <optional>
#include <string>

std::optional<double> compute_field(
    const std::string& expression,
    const std::function<double(const std::string&)>& lookup);
