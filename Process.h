#pragma once
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <string>
#include <fstream>
#include <atomic>
#include <iomanip>
#include <map>
#include <queue>
#include <memory>
#include <chrono>
#include <sstream>
#include <ctime>
#include <random>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <unordered_map>

// timestamp used by Process and the print records, ex: 08/06/2024 09:15:22AM
inline std::string getTimestamp()
{
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t); // windows
#else
    localtime_r(&t, &tm); // linux/mac
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%m/%d/%Y %I:%M:%S%p");
    return oss.str();
}

enum class Instruction
{
    PRINT,
    DECLARE,
    ADD,
    SUBTRACT,
    SLEEP,
    FOR
};

// Instruction parameters and attributes
struct Attributes
{
    Instruction type;
    std::string var1; // instructions can accept 3 args depending on type
    std::string var2;
    std::string var3;
    uint16_t value = 0;
    std::vector<Attributes> loopedTasks; // for FOR instruction
};

// Process struct (name, submit time, total commands, remaining commands, assigned core, finished flag)
struct Process
{
    std::string id;
    std::string submitTime;
     
    uint64_t totalCommands = 0;
    std::atomic<uint64_t> remainingCommands{0};
    std::atomic<int> assignedCore{-1};
    std::atomic<bool> isFinished{false};
    std::atomic<bool> isSleeping{false};
    std::atomic<uint8_t> sleepTicksRemaining{0};

    std::unordered_map<std::string, uint16_t> variables;

    std::vector<Attributes> instructions;
    std::atomic<size_t> programCounter{0};

    // PRINT logs accessible through process-smi
    std::vector<std::string> logs;
    std::mutex logsMutex;

    Process(std::string pid, std::vector<Attributes> instructionsList = {})
        : id(pid), instructions(std::move(instructionsList))
    {
        submitTime = getTimestamp(); // record submission time

        totalCommands = instructions.size(); // set total commands
        remainingCommands = totalCommands;  // set remaining commands
    }

    uint16_t evaluateArgument(const std::string &arg)
    {
        // skips empty args
        if (arg.empty())
            return 0;

        // if arg is a number, return its value
        if (std::all_of(arg.begin(), arg.end(), [](unsigned char c)
                { return std::isdigit(c); }))
        {
            unsigned long val = std::stoul(arg);
            return static_cast<uint16_t>(std::min(val, 65535UL));
        }

        // automatically declare as 0 if missing in symbol table
        if (variables.find(arg) == variables.end())
        {
            variables[arg] = 0;
        }

        // if arg != numerical, treat arg as varname in symbol table
        return variables[arg];
    }

    // clamp values [0, 65535]
    uint16_t clamp(long long val)
    {
        if (val < 0)
            return 0;
        if (val > 65535)
            return 65535;
        return static_cast<uint16_t>(val);
    }

    // Variable assigner to symbol table
    void setVariable(const std::string &varName, long long val)
    {
        variables[varName] = clamp(val);
    }
};

// gen instructions for sample processes
inline std::vector<Attributes> generateSampleInstructions(int offset, uint64_t numInstruction)
{
    // randomized seed
    std::mt19937 rng(std::random_device{}() + offset);

    std::uniform_int_distribution<int> opDist(0, 4);    // Instruction choice
    std::uniform_int_distribution<int> valDist(1, 100); // Value range
    std::uniform_int_distribution<int> sleepDist(1, 5); // Sleep ticks
    std::uniform_int_distribution<int> loopDist(2, 4);  // Loop repeats
    

    std::vector<std::string> varNames = {"x", "y", "z", "counter"};
    auto getRandomVar = [&](std::mt19937 &gen)
    {
        std::uniform_int_distribution<size_t> varDist(0, varNames.size() - 1);
        return varNames[varDist(gen)];
    };

    std::vector<Attributes> instrs;
    uint64_t numInstructions = numInstruction; 

    for (uint64_t i = 0; i < numInstructions; ++i)
    {
        int opChoice = opDist(rng);

        switch (opChoice)
        {
        case 0: // DECLARE
            instrs.push_back({Instruction::DECLARE, getRandomVar(rng), std::to_string(valDist(rng))});
            break;

        case 1: // ADD
            instrs.push_back({Instruction::ADD, getRandomVar(rng), getRandomVar(rng), std::to_string(valDist(rng))});
            break;

        case 2: // SUBTRACT
            instrs.push_back({Instruction::SUBTRACT, getRandomVar(rng), getRandomVar(rng), std::to_string(valDist(rng))});
            break;

        case 3: // PRINT
            if (valDist(rng) % 2 == 0)
            {
                instrs.push_back({Instruction::PRINT, "Value of " + getRandomVar(rng) + ": ", getRandomVar(rng)});
            }
            else
            {
                instrs.push_back({Instruction::PRINT, "", ""}); // Default "Hello world from <pid>!"
            }
            break;

        case 4: // SLEEP
            instrs.push_back({Instruction::SLEEP, std::to_string(sleepDist(rng))});
            break;
        }
    }
    // ]randomized nested FOR loop (2 levels here, capped at 3)
    Attributes innerPrint{Instruction::PRINT, "", ""};
    Attributes innerLoop{Instruction::FOR, "", "", "", static_cast<uint16_t>(loopDist(rng)), {innerPrint}};
    Attributes outerLoop{Instruction::FOR, "", "", "", static_cast<uint16_t>(loopDist(rng)), {innerLoop}};

    instrs.push_back(outerLoop);

    return instrs;
}