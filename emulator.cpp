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
#include <algorithm>
#include <unordered_map>
#include <cstdint>
#include <limits>
#include <random>
#include "Process.h"
#include "Config.h"


std::queue<std::shared_ptr<Process>> readyQueue; // FCFS
std::mutex queueMutex;

std::vector<std::shared_ptr<Process>> runningList; // processes on a core right now
std::mutex runningMutex;

std::vector<std::shared_ptr<Process>> finishedList; // processes that are done
std::mutex finishedMutex;

std::vector<std::shared_ptr<Process>> sleepingList;
std::mutex sleepingMutex;

std::condition_variable queueCV;                   // wakes up the workers; will be used when exiting
std::map<std::string, std::ofstream> processFiles; // one file per process
std::mutex fileMutex;

std::map<std::string, std::shared_ptr<Process>> allProcesses; //MODIFIED
std::mutex allProcessesMutex; 

std::atomic<bool> emulatorRunning{true}; // set to false on exit

// Core assignments state to manage exactly 4 worker threads
std::vector<std::shared_ptr<Process>> coreAssignments;
std::mutex coreMutex;

const std::string PROMPT = "root:\\> ";

Config config;
std::atomic<bool> initialized{false};
std::atomic<bool> generatingProcesses{false};
std::atomic<uint64_t> cpuCycles{0};
std::atomic<uint64_t> frameCounter{0};
std::atomic<int> processCounter{0};
std::atomic<uint64_t> nextProcessTick{0};

std::vector<std::thread> workers;
std::thread schedulerThreadHandle;
std::thread cpuTickThreadHandle;

void writePrintRecord(std::shared_ptr<Process> process, int core, const std::string &msg)
{
    std::string line = "(" + getTimestamp() + ") Core:" + std::to_string(core) + " \"" + msg + "\"";

    {
        std::lock_guard<std::mutex> lock(process->logsMutex);
        process->logs.push_back(line);
    }

    std::lock_guard<std::mutex> lock(fileMutex);

    auto it = processFiles.find(process->id);

    if (it == processFiles.end())
    {
        std::ofstream f(process->id + "_output.txt", std::ios::out | std::ios::trunc);

        if (!f.is_open())
            return;

        f << "Process name: " << process->id << "\nLogs:\n\n";
        it = processFiles.emplace(process->id, std::move(f)).first;
    }

    it->second << line << "\n" << std::flush;
}

// declaration for executeInstructionList
bool executeInstruction(std::shared_ptr<Process> process, int coreId, const Attributes &instruction, int depth);

// runs a list of instructions sequentially for FOR
bool executeInstructionList(std::shared_ptr<Process> proc, int coreId, const std::vector<Attributes> &list, int depth)
{
    for (const auto &instr : list)
    {
        if (!executeInstruction(proc, coreId, instr, depth))
        {
            return false;
        }
    }
    return true;
}

bool executeInstruction(std::shared_ptr<Process> process, int coreId, const Attributes &instruction, int depth)
{
    // 1 Clock tick; wait for the next CPU frame pass
    uint64_t startTick = frameCounter.load();
    while (frameCounter.load() == startTick && emulatorRunning)
    {
        std::this_thread::yield();
    }

    switch (instruction.type)
    {
    case Instruction::PRINT: // PRINT instruction
    {
        std::string outputString = instruction.var1; // get var1

        // evaluate var2 if any
        if (!instruction.var2.empty())
        {
            uint16_t val = process->evaluateArgument(instruction.var2);
            outputString += std::to_string(val);
        }

        if (outputString.empty())
        {
            // default message if no arg passed
            outputString = "Hello World from " + process->id + "!";
        }
         writePrintRecord(process, coreId, outputString);
        break;
    }
    case Instruction::DECLARE:
    {
        // assign to symbol table var and value
        process->setVariable(instruction.var1, process->evaluateArgument(instruction.var2));
        break;
    }
    case Instruction::ADD:
    {
        uint16_t op1 = process->evaluateArgument(instruction.var2);
        uint16_t op2 = process->evaluateArgument(instruction.var3);
        process->setVariable(instruction.var1, op1 + op2); // put result to symbol table w/ name
        break;
    }
    case Instruction::SUBTRACT:
    {
        uint16_t op1 = process->evaluateArgument(instruction.var2);
        uint16_t op2 = process->evaluateArgument(instruction.var3);
        process->setVariable(instruction.var1, op1 - op2);
        break;
    }
    case Instruction::SLEEP:
    {
        process->sleepTicksRemaining = static_cast<uint8_t>(process->evaluateArgument(instruction.var1));
        process->isSleeping = true;
        process->remainingCommands--;
        return false;
    }
    case Instruction::FOR:
    {
        if (depth >= 3) // only 3 loops
        {
            // Max loop reached, stop
            break;
        }
        for (uint16_t i = 0; i < instruction.value; ++i)
        {
            if (!executeInstructionList(process, coreId, instruction.loopedTasks, depth + 1))
            {
                return false;
            }
        }
        break;
    }
    }

    process->remainingCommands--;
    
    return true;
}


void cpuTickThread()
{
    while (emulatorRunning)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        uint64_t tick = ++cpuCycles;
        frameCounter = tick;

        // scheduler-start batch generation: every config.batchProcessFreq ticks
        if (generatingProcesses && config.batchProcessFreq > 0 &&
            tick >= nextProcessTick)
        {
            nextProcessTick = tick + config.batchProcessFreq;
            ++processCounter;
            std::ostringstream name;
            name << "p" << std::setw(2) << std::setfill('0') << processCounter.load();

            std::mt19937 rng(std::random_device{}() + processCounter.load());
            std::uniform_int_distribution<uint64_t> insDist(config.minIns, config.maxIns);

            uint64_t numInstructions = insDist(rng);

            auto instrs = generateSampleInstructions(processCounter.load(), numInstructions);
            auto proc = std::make_shared<Process>(name.str(), instrs);

            // Add to allProcesses map
            {
                std::lock_guard<std::mutex> lock(allProcessesMutex);
                allProcesses[proc->id] = proc;
            }

            // Add to ready queue
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                readyQueue.push(proc);
            }

            queueCV.notify_all();
        }
    }
}

void workerThread(int coreId)
{
    // 1 thread per CPU worker
    while (emulatorRunning)
    {
        std::shared_ptr<Process> currentProc = nullptr;
        {
            std::unique_lock<std::mutex> lock(coreMutex);
            queueCV.wait(lock, [coreId]()
                         { return !emulatorRunning || coreAssignments[coreId] != nullptr; });

            if (!emulatorRunning && coreAssignments[coreId] == nullptr)
            {
                break;
            }

            currentProc = coreAssignments[coreId];
        }

        if (currentProc)
        {
            bool yielded = false;
            bool isSleep = false;
            uint64_t quantumUsed = 0;

            while (currentProc->programCounter < currentProc->instructions.size())
            {
                const auto &instr = currentProc->instructions[currentProc->programCounter];
                bool continueExec = executeInstruction(currentProc, coreId, instr, 0);

                currentProc->programCounter++;
                ++quantumUsed;

                if (!continueExec) // SLEEP was executed
                {
                    yielded = true;
                    isSleep = true;
                    break;
                }

                // RR: preempt if quantum exhausted
                if (config.scheduler == "rr" && quantumUsed >= config.quantumCycles)
                {
                    yielded = true;
                    break;
                }
            }

            // Only mark finished if the program counter actually reached the end
            bool reallyFinished =
                (currentProc->programCounter >= currentProc->instructions.size());

            if (reallyFinished)
            {
                currentProc->isFinished = true;
            }

            // Remove process from runningList
            {
                std::lock_guard<std::mutex> lock(runningMutex);
                runningList.erase(
                    std::remove(runningList.begin(), runningList.end(), currentProc),
                    runningList.end());
            }

            if (isSleep)
            {
                std::lock_guard<std::mutex> lock(sleepingMutex);
                sleepingList.push_back(currentProc);
            }
            else if (reallyFinished)
            {
                std::lock_guard<std::mutex> lock(finishedMutex);
                finishedList.push_back(currentProc);
            }
            else
            {
                // RR quantum preemption: back to ready queue
                std::lock_guard<std::mutex> lock(queueMutex);
                readyQueue.push(currentProc);
                queueCV.notify_all();
            }

            // Free assigned core
            {
                std::lock_guard<std::mutex> lock(coreMutex);
                coreAssignments[coreId] = nullptr;
            }
            queueCV.notify_all();
        }
    }
}

void schedulerThread()
{
    while (emulatorRunning)
    {
        // sleeping queue
        {
            std::lock_guard<std::mutex> sleepLock(sleepingMutex);
            for (auto it = sleepingList.begin(); it != sleepingList.end();)
            {
                auto &proc = *it;
                if (proc->sleepTicksRemaining > 0)
                {
                    proc->sleepTicksRemaining--;
                }

                if (proc->sleepTicksRemaining == 0)
                {
                    proc->isSleeping = false;
                    {
                        std::lock_guard<std::mutex> qLock(queueMutex);
                        readyQueue.push(proc);
                    }
                    it = sleepingList.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        std::shared_ptr<Process> currentProc = nullptr;
        int idleCore = -1;

        // check for a core and a process
        {
            // lock both mutexes
            std::lock(coreMutex, queueMutex);
            std::lock_guard<std::mutex> lockCore(coreMutex, std::adopt_lock);
            std::lock_guard<std::mutex> lockQueue(queueMutex, std::adopt_lock);

            // check if core i is available
            if (!readyQueue.empty())
            {
                for (int i = 0; i < static_cast<int>(coreAssignments.size()); ++i)
                {
                    if (coreAssignments[i] == nullptr)
                    {
                        idleCore = i;
                        break;
                    }
                }

                // FIFO - assign core to process
                if (idleCore != -1)
                {
                    currentProc = readyQueue.front();
                    readyQueue.pop();

                    currentProc->assignedCore = idleCore;
                    coreAssignments[idleCore] = currentProc;
                }
            }
        }

        // if process was assigned, add to runningList
        if (currentProc != nullptr)
        {
            {
                std::lock_guard<std::mutex> lock(runningMutex);
                runningList.push_back(currentProc);
            }

            // call other threads
            queueCV.notify_all();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

//MODIFIED
std::string generateSystemReport()
{
    std::ostringstream oss;

    int coresUsed = 0;
    int coresTotal = 0;

    {
        std::lock_guard<std::mutex> lock(coreMutex);

        coresTotal = static_cast<int>(coreAssignments.size());

        for (auto &core : coreAssignments)
        {
            if (core != nullptr)
                coresUsed++;
        }
    }

    int utilization = (coresTotal > 0)
                          ? (coresUsed * 100 / coresTotal)
                          : 0;

    oss << "CPU utilization: " << utilization << "%\n";
    oss << "Cores used: " << coresUsed << "\n";
    oss << "Cores available: " << coresTotal - coresUsed << "\n";
    oss << "----------------------------------------\n";

    oss << "Running processes:\n";

    {
        std::lock_guard<std::mutex> lock(allProcessesMutex);
        for (auto &entry : allProcesses)
        {
            auto &p = entry.second;
            if (!p->isFinished)
            {
                std::string status;
                if (p->isSleeping)
                    status = "Sleeping";
                else if (p->assignedCore.load() >= 0)
                    status = "Core: " + std::to_string(p->assignedCore.load());
                else
                    status = "Waiting";

                oss << std::left << std::setw(10) << p->id
                    << "(" << p->submitTime << ")   "
                    << std::setw(12) << status
                    << p->programCounter.load()
                    << " / " << p->instructions.size() << "\n";
            }
        }
    }

    oss << "\nFinished processes:\n";

    {
        std::lock_guard<std::mutex> lock(allProcessesMutex);
        for (auto &entry : allProcesses)
        {
            auto &p = entry.second;
            if (p->isFinished)
            {
                oss << std::left << std::setw(10) << p->id
                    << "(" << p->submitTime << ")   "
                    << std::setw(12) << "Finished"
                    << p->instructions.size()
                    << " / " << p->instructions.size() << "\n";
            }
        }
    }

    oss << "----------------------------------------\n";
    return oss.str();
}

void cmdScreenLs()
{
    std::cout << generateSystemReport() << PROMPT;
}

void cmdReportUtil()
{
    std::string report = generateSystemReport();
    std::ofstream outFile("csopesy-log.txt", std::ios::out | std::ios::trunc);

    if (outFile.is_open())
    {
        outFile << report;
        outFile.close();
        std::cout << "Report generated at csopesy-log.txt!\n> " << PROMPT;
    }
    else
    {
        std::cout << "Failed to generate report file.\n> " << PROMPT;
    }
}

// "Type "exit" to close your emulator."
void shutdownEmulator()
{
    std::cout << "\nShutting down...\n";
    emulatorRunning = false;
    queueCV.notify_all(); // wake up sleeping workers to stop for shutting down
}

// Attach to a process screen for viewing logs and process-smi
void attachProcessScreen(std::shared_ptr<Process> process)
{
    std::string command;

    std::cout << "\nAttached to process: " << process->id << "\n";
    std::cout << "Enter 'process-smi' to view process information or 'exit' to return.\n";

    while (emulatorRunning && !process->isFinished)
    {
        std::cout << process->id << "> ";

        if (!std::getline(std::cin, command))
            return;

        if (command == "exit")
        {
            std::cout << "Detached from " << process->id << ".\n";
            return;
        }
        else if (command == "process-smi")
        {
            std::cout << "\nProcess name: " << process->id << "\n";
            std::cout << "ID: " << process->id << "\n";
            std::cout << "Current instruction: "
                      << process->programCounter.load()
                      << " / " << process->instructions.size() << "\n";
            std::cout << "Remaining instructions: "
                      << process->remainingCommands.load() << "\n";

            if (process->assignedCore.load() >= 0)
                std::cout << "Assigned core: " << process->assignedCore.load() << "\n";
            else
                std::cout << "Assigned core: None\n";

            std::cout << "\nLogs:\n";

            {
                std::lock_guard<std::mutex> lock(process->logsMutex);

                if (process->logs.empty())
                {
                    std::cout << "No logs yet.\n";
                }
                else
                {
                    for (const auto &log : process->logs)
                        std::cout << log << "\n";
                }
            }

            if (process->isFinished)
                std::cout << "\nFinished!\n";

            std::cout << "\n";
        }
        else
        {
            std::cout << "Invalid command. Available commands: process-smi | exit\n";
        }
    }

    if (process->isFinished)
        std::cout << "\nFinished!\n";
}


int main()
{
    

    std::cout << "Welcome to CSOPESY Emulator!\n";
    std::cout << "Developers:\n Bactong, Gabrielle Joei V.\n Lee, Ashley Fiona S. \n Lobo, Shirley Marie A. \n Roldan, Beatrice C.";
    std::cout << "Last updated: 11-02-2026\n\n";

    std::cout << "> ";
    std::string input;
    while (emulatorRunning && std::getline(std::cin, input))
    {
        if (!initialized && input != "initialize" && input != "exit")
        {
            std::cout << "Please run 'initialize' first.\n> ";
        }
        else if (input == "initialize")
        {
            std::string err;
            if (!loadConfig("config.txt", config, err))
            {
                std::cout << "Config error: " << err << "\n> ";
            }
            else
            {
                if (workers.empty())
                {
                    coreAssignments.assign(config.numCpu, nullptr);
                    for (uint64_t i = 0; i < config.numCpu; ++i)
                        workers.emplace_back(workerThread, static_cast<int>(i));

                    schedulerThreadHandle = std::thread(schedulerThread);
                    cpuTickThreadHandle = std::thread(cpuTickThread);
                }

                initialized = true;
                std::cout << "Initialized with " << config.numCpu << " cores.\n> ";
            }
        }
        else if (input == "scheduler-start")
        {
            if (!initialized) {
                std::cout << "Please run 'initialize' first.\n> ";
            } else {
                generatingProcesses = true;
                nextProcessTick = cpuCycles.load() + config.batchProcessFreq;
                std::cout << "Scheduler started.\n> ";
            }
        }
        else if (input == "scheduler-stop")
        {
            generatingProcesses = false;
            std::cout << "Scheduler stopped.\n> ";
        }
        else if (input == "screen -ls")
        {
            cmdScreenLs();
        }
        else if (input == "report-util")
        {
            cmdReportUtil();
        }
        else if (input.rfind("screen -s ", 0) == 0)
        {
            std::string pname = input.substr(10);
            if (pname.empty()) {
                std::cout << "Usage: screen -s <process name>\n> ";
            } else {
                std::lock_guard<std::mutex> lock(runningMutex);
                auto it = std::find_if(runningList.begin(), runningList.end(),
                                        [&pname](const std::shared_ptr<Process> &p) {
                                            return p->id == pname;
                                        });
                if (it != runningList.end()) {
                    attachProcessScreen(*it);
                } else {
                    std::cout << "Process " << pname << " not found.\n> ";
                }
            }
        }
        else if (input.rfind("screen -r ", 0) == 0)
        {
            std::string pname = input.substr(10);
            if (pname.empty()) {
                std::cout << "Usage: screen -r <process name>\n> ";
            } else {
                bool found = false;
                {
                    std::lock_guard<std::mutex> lock(runningMutex);
                    for (auto &p : runningList)
                        if (p->id == pname) { found = true; break; }
                }
                if (found)
                    std::cout << "Resumed screen of " << pname << "\n> ";
                else
                    std::cout << "Process " << pname << " not found.\n> ";
            }
        }
        else if (input == "exit")
        {
            shutdownEmulator();
            break;
        }
        else
        {
            std::cout << "Available commands: initialize | screen -s <name> | screen -r <name> | screen -ls | scheduler-start | scheduler-stop | report-util | exit\n> ";
        }
    }

    // wait for all threads to stop first
    emulatorRunning = false;
    queueCV.notify_all();

    if (cpuTickThreadHandle.joinable())
        cpuTickThreadHandle.join();

    for (auto &t : workers)
    {
        if (t.joinable())
            t.join();
    }

    if (schedulerThreadHandle.joinable())
        schedulerThreadHandle.join();

    // close the files only after the threads are done
    {
        std::lock_guard<std::mutex> lock(fileMutex);
        for (auto &f : processFiles)
            if (f.second.is_open())
                f.second.close();
        processFiles.clear();
    }

    std::cout << "Done.\n";
    return 0;
}
