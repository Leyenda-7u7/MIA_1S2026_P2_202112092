#include <iostream>
#include <string>
#include <filesystem>

#include "CommandParser.hpp"

int main() {
    std::string line;

    std::cout << "Backend Proyecto 1 - Consola\n";
    std::cout << "Escribe 'exit' para salir\n\n";

    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) break;

        std::string out = executeLine(line);

        if (out == "EXIT") break;
        if (!out.empty()) std::cout << out << "\n";
    }

    return 0;

    
}