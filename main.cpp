// Copyright [2025] <Antoni Gorski> <agorski@student.42warsaw.pl>
// Copyright [2025] <Piotr Ruszkiewicz> <pruszkie@student.42warsaw.pl>
// Copyright [2025] <Grzegorz Buczynski> <gbuczyns@student.42warsaw.pl>

#include "coreEngine/CoreEngine.hpp"
#include "configReader/config.hpp"
#include "http/Http.hpp"
#include "utils/utils.hpp"

#include <iostream>
#include <cerrno>
#include <cstring>
#include <csignal>

volatile sig_atomic_t CoreEngine::alive = true;

void signalHandler(int sigNum)
{
    (void)sigNum;
    CoreEngine::alive = false;
}

int main(int argc, char **argv)
{
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

try
    {
        
    std::cout << "Starting configuration parsing...\n";

    std::string filePath = inputCheck(argc, argv);
    Config config(filePath);
    const std::vector<ServerConfig> &servers = config.getServers();
    
    std::cout << "Configuration loaded successfully!\n";
    config.printConfigs();
    
    std::cout << "\nStarting CoreEngine...\n";
    CoreEngine engine(servers);
    engine.coreEngine(); // ta funkcja nigdy nie powinna zwracac bledow na zewnatrz
    /**
     * try-catch w coreEngine() dla całej iteracji pętli
     * try-catch w recivNClose() dla każdego klienta osobno
     * return zamiast exit() — nie zabijaj serwera
     * Zawsze zamykaj socket przy błędzie
     */

    }
catch(const std::exception& e)
{
    std::cerr << "Main Error: " << e.what() << '\n';
    return 1;
}
return 0;
}

//tickl