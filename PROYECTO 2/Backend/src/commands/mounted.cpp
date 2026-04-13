#include "commands/mounted.hpp"
#include "commands/mount_table.hpp"
#include <sstream>

namespace cmd {

bool mounted(std::string& outMsg) {
    const auto& list = mount_table::list();

    if (list.empty()) {
        outMsg = "No hay particiones montadas en memoria.";
        return true;
    }

    std::ostringstream oss;
    // Formato tipo: 921A, 922A, 921B
    for (size_t i = 0; i < list.size(); i++) {
        oss << list[i].id;
        if (i + 1 < list.size()) oss << ", ";
    }

    outMsg = oss.str();
    return true;
}

}