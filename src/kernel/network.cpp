#include "network.h"
#include "networkserver.h"
#include "networkclient.h"

CryptoKernel::Network::Network(CryptoKernel::Log* log, CryptoKernel::Blockchain* blockchain)
{
    this->log = log;
    this->blockchain = blockchain;

    httpServer.reset(new jsonrpc::HttpServer(49000));
    server.reset(new Server(*httpServer.get()));
    server->setBlockchain(blockchain);
    server->StartListening();

    peers = new CryptoKernel::Storage("./peers");

    Json::Value seeds = peers->get("seeds");
    if(seeds.size() <= 0)
    {
        std::ifstream infile("peers.txt");
        if(!infile.is_open())
        {
            throw std::runtime_error("Could not open peers file");
        }

        std::string line;
        while(std::getline(infile, line))
        {
            Json::Value newSeed;
            newSeed["url"] = line;
            newSeed["lastseen"] = -1;
            newSeed["height"] = 1;
            seeds.append(newSeed);
        }

        infile.close();

        peers->store("seeds", seeds);
    }

    if(seeds.size() <= 0)
    {
        throw std::runtime_error("There are no known peers to connect to");
    }

    running = true;

    // Start management thread
    networkThread.reset(new std::thread(&CryptoKernel::Network::networkFunc, this));
}

CryptoKernel::Network::~Network()
{
    running = false;
    networkThread->join();
    server->StopListening();
    delete peers;

    for(std::map<std::string, Peer*>::iterator it = connected.begin(); it != connected.end(); it++)
    {
        delete it->second;
    }
}

void CryptoKernel::Network::networkFunc()
{
    while(running)
    {
        Json::Value seeds = peers->get("seeds");

        if(connected.size() < 8)
        {
            for(unsigned int i = 0; i < seeds.size(); i++)
            {
                if(connected.size() >= 8)
                {
                    break;
                }

                std::map<std::string, Peer*>::iterator it = connected.find(seeds[i]["url"].asString());
                if(it != connected.end())
                {
                    continue;
                }

                Peer* peer = new Peer;
                // Attempt to connect to peer
                peer->client.reset(new Client(seeds[i]["url"].asString()));
                // Get height
                Json::Value info;
                try
                {
                    log->printf(LOG_LEVEL_INFO, "Network(): Attempting to connect to " + seeds[i]["url"].asString());
                    info = peer->client->getInfo();
                }
                catch(jsonrpc::JsonRpcException& e)
                {
                    log->printf(LOG_LEVEL_WARN, "Network(): Failed to connect to " + seeds[i]["url"].asString());
                    continue;
                }

                log->printf(LOG_LEVEL_INFO, "Network(): Successfully connected to " + seeds[i]["url"].asString());

                // Update info
                seeds[i]["height"] = info["tipHeight"];

                std::time_t result = std::time(nullptr);
                seeds[i]["lastseen"] = std::asctime(std::localtime(&result));

                peer->info = seeds[i];

                connected[seeds[i]["url"].asString()] = peer;
            }
        }

        for(std::map<std::string, Peer*>::iterator it = connected.begin(); it != connected.end(); it++)
        {
            try
            {
                const Json::Value info = it->second->client->getInfo();
                it->second->info["height"] = info["tipHeight"];
                std::time_t result = std::time(nullptr);
                it->second->info["lastseen"] = std::asctime(std::localtime(&result));
            }
            catch(jsonrpc::JsonRpcException& e)
            {
                delete it->second;
                it = connected.erase(it);
            }
        }

        peers->store("seeds", seeds);

        //Determine best chain
        uint64_t currentHeight = blockchain->getBlock("tip").height;
        uint64_t bestHeight = currentHeight;
        for(std::map<std::string, Peer*>::iterator it = connected.begin(); it != connected.end(); it++)
        {
            if(it->second->info["height"].asUInt64() > bestHeight)
            {
                bestHeight = it->second->info["height"].asUInt64();
            }
        }

        log->printf(LOG_LEVEL_INFO, "Network(): Current height: " + std::to_string(currentHeight) + ", best height: " + std::to_string(bestHeight));

        //Detect if we are behind
        if(bestHeight > currentHeight)
        {
            for(std::map<std::string, Peer*>::iterator it = connected.begin(); it != connected.end(); it++)
            {
                if(it->second->info["height"].asUInt64() > currentHeight)
                {
                    log->printf(LOG_LEVEL_INFO, "Network(): Downloading blocks " + std::to_string(currentHeight + 1) + " to " + std::to_string(currentHeight + 201));
                    std::vector<CryptoKernel::Blockchain::block> blocks = it->second->client->getBlocks(currentHeight + 1, currentHeight + 201);
                    for(CryptoKernel::Blockchain::block block : blocks)
                    {
                        if(!blockchain->submitBlock(block))
                        {
                            break;
                        }
                    }
                    break;
                }
            }
        }

        //Rebroadcast unconfirmed transactions
        broadcastTransactions(blockchain->getUnconfirmedTransactions());

        std::this_thread::sleep_for(std::chrono::milliseconds(10000));
    }
}

unsigned int CryptoKernel::Network::getConnections()
{
    return connected.size();
}

void CryptoKernel::Network::broadcastTransactions(const std::vector<CryptoKernel::Blockchain::transaction> transactions)
{
    for(std::map<std::string, Peer*>::iterator it = connected.begin(); it != connected.end(); it++)
    {
        it->second->client->sendTransactions(transactions);
    }
}

void CryptoKernel::Network::broadcastBlock(const CryptoKernel::Blockchain::block block)
{
    for(std::map<std::string, Peer*>::iterator it = connected.begin(); it != connected.end(); it++)
    {
        it->second->client->sendBlock(block);
    }
}
