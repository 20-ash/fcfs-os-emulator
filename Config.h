#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

struct Config
{
    uint64_t numCpu = 4;           
    std::string scheduler = "rr";  
    uint64_t quantumCycles = 5;  
    uint64_t batchProcessFreq = 1;
    uint64_t minIns = 1000;      
    uint64_t maxIns = 2000;       
    uint64_t delaysPerExec = 0;   
};

constexpr uint64_t CONFIG_MAX = 1ULL << 32; 

inline bool parseConfigUInt(const std::string &s, uint64_t &out)
{
    if (s.empty() || s.size() > 19)
        return false;
    for (char c : s)
    {
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    }
    out = std::stoull(s);
    return true;
}

inline bool loadConfig(const std::string &path, Config &out, std::string &error)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        error = "could not open " + path;
        return false;
    }

    Config cfg;
    std::string line;
    int lineNo = 0;

    while (std::getline(file, line))
    {
        ++lineNo;
        std::istringstream iss(line);
        std::string key, value, extra;

        if (!(iss >> key))
            continue;

        const std::string where = path + " line " + std::to_string(lineNo) + ": ";

        if (!(iss >> value))
        {
            error = where + "missing value for '" + key + "'";
            return false;
        }
        if (iss >> extra)
        {
            error = where + "unexpected extra token '" + extra + "'";
            return false;
        }

        if (key == "scheduler")
        {
            value.erase(std::remove(value.begin(), value.end(), '"'), value.end());
            std::transform(value.begin(), value.end(), value.begin(),
                            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (value != "fcfs" && value != "rr")
            {
                error = where + "'scheduler' must be \"fcfs\" or \"rr\"";
                return false;
            }
            cfg.scheduler = value;
            continue;
        }

        uint64_t num = 0;
        if (!parseConfigUInt(value, num))
        {
            error = where + "'" + key + "' must be a non-negative integer";
            return false;
        }

        auto assignIfInRange = [&](uint64_t &target, uint64_t lo, uint64_t hi, const std::string &rangeStr)
        {
            if (num < lo || num > hi)
            {
                error = where + "'" + key + "' must be in " + rangeStr;
                return false;
            }
            target = num;
            return true;
        };

        if (key == "num-cpu")
        {
            if (!assignIfInRange(cfg.numCpu, 1, 128, "[1, 128]"))
                return false;
        }
        else if (key == "quantum-cycles")
        {
            if (!assignIfInRange(cfg.quantumCycles, 1, CONFIG_MAX, "[1, 2^32]"))
                return false;
        }
        else if (key == "batch-process-freq")
        {
            if (!assignIfInRange(cfg.batchProcessFreq, 1, CONFIG_MAX, "[1, 2^32]"))
                return false;
        }
        else if (key == "min-ins")
        {
            if (!assignIfInRange(cfg.minIns, 1, CONFIG_MAX, "[1, 2^32]"))
                return false;
        }
        else if (key == "max-ins")
        {
            if (!assignIfInRange(cfg.maxIns, 1, CONFIG_MAX, "[1, 2^32]"))
                return false;
        }
        else if (key == "delay-per-exec" || key == "delays-per-exec")
        {
            if (!assignIfInRange(cfg.delaysPerExec, 0, CONFIG_MAX, "[0, 2^32]"))
                return false;
        }
        else
        {
            error = where + "unknown parameter '" + key + "'";
            return false;
        }
    }

    if (cfg.minIns > cfg.maxIns)
    {
        error = "min-ins must not be greater than max-ins";
        return false;
    }

    out = cfg;
    return true;
}