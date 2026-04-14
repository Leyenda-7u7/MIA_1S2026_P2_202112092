#include "commands/unmount.hpp"
#include "commands/mount.hpp"
#include "Structures.hpp"

#include <fstream>
#include <cstring>

namespace cmd {

bool unmount(const std::string& id, std::string& outMsg) {

    if (id.empty()) {
        outMsg = "Error: unmount requiere -id.";
        return false;
    }

    std::string diskPath;
    int32_t partStart = 0;
    int32_t partSize = 0;
    std::string err;

    // 1. Buscar partición montada
    if (!getMountedById(id, diskPath, partStart, partSize, err)) {
        outMsg = "Error: no existe una partición montada con id: " + id;
        return false;
    }

    // 2. Abrir disco
    std::fstream file(diskPath, std::ios::binary | std::ios::in | std::ios::out);
    if (!file.is_open()) {
        outMsg = "Error: no se pudo abrir el disco.";
        return false;
    }

    // 3. Leer MBR
    MBR mbr{};
    file.seekg(0);
    file.read(reinterpret_cast<char*>(&mbr), sizeof(MBR));

    // 4. Buscar partición por start
    bool found = false;

    for (int i = 0; i < 4; i++) {
        Partition& p = mbr.mbr_partitions[i];

        if (p.part_start == partStart) {
            p.part_correlative = 0; 
            std::memset(p.part_id, 0, sizeof(p.part_id));
            found = true;
            break;
        }
    }

    if (!found) {
        outMsg = "Error: no se encontró la partición en el MBR.";
        return false;
    }

    // 5. Guardar MBR
    file.seekp(0);
    file.write(reinterpret_cast<char*>(&mbr), sizeof(MBR));

    // 6. Eliminar de la lista de montadas
    removeMountedById(id); 

    outMsg = "Partición desmontada correctamente: " + id;
    return true;
}

}