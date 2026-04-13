#include "third_party/httplib.h"
#include "CommandParser.hpp"

#include <iostream>
#include <sstream>
#include <string>

using namespace httplib;

static std::string ejecutarComandos(const std::string& input) {
    std::stringstream ss(input);
    std::string line;
    std::stringstream salida;

    while (std::getline(ss, line)) {
        if (line.empty()) {
            salida << "\n";
            continue;
        }

        std::string result = executeLine(line);

        if (result == "EXIT") {
            salida << "EXIT\n";
            break;
        }

        if (!result.empty()) {
            salida << result << "\n";
        }
    }

    return salida.str();
}

int main() {
    Server svr;

    // CORS
    svr.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Headers", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"}
    });

    // Responder preflight de CORS
    svr.Options(R"(.*)", [](const Request&, Response& res) {
        res.status = 200;
    });

    // Ruta simple para probar en navegador
    svr.Get("/", [](const Request&, Response& res) {
        res.set_content("Servidor Backend MIA funcionando", "text/plain; charset=utf-8");
    });

    // Endpoint real para ejecutar comandos
    svr.Post("/exec", [](const Request& req, Response& res) {
        std::string salida = ejecutarComandos(req.body);
        res.set_content(salida, "text/plain; charset=utf-8");
    });

    std::cout << "Servidor corriendo en http://localhost:8080\n";
    svr.listen("0.0.0.0", 8080);
}