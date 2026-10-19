#include "abss/oracle/oracle.hpp"

namespace abss {

std::vector<std::int32_t> DofMap::freeIndices() const {
    std::vector<std::int32_t> out;
    for (std::int32_t i = 0; i < size(); ++i)
        if (!prescribed[static_cast<size_t>(i)]) out.push_back(i);
    return out;
}

std::vector<std::int32_t> DofMap::prescribedIndices() const {
    std::vector<std::int32_t> out;
    for (std::int32_t i = 0; i < size(); ++i)
        if (prescribed[static_cast<size_t>(i)]) out.push_back(i);
    return out;
}

}  // namespace abss
