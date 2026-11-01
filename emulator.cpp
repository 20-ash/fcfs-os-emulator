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
#include "Process.h"

const int NUM_CORES = 4;

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
std::atomic<bool> emulatorRunning{true}; // set to false on exit

// Core assignments state to manage exactly 4 worker threads
std::vector<std::shared_ptr<Process>> coreAssignments(NUM_CORES, nullptr);
std::mutex coreMutex;

void writePrintRecord(const std::string &pid, int core, const std::string &msg)
{
    std::string line = "(" + getTimestamp() + ") Core:" + std::to_string(core) + " \"" + msg + "\"";
    std::lock_guard<std::mutex> lock(fileMutex);

    auto it = processFiles.find(pid);
    if (it == processFiles.end())
    {
        std::ofstream f(pid + "_output.txt", std::ios::out | std::ios::trunc);
        if (!f.is_open())
            return;
        f << "Process name: " << pid << "\nLogs:\n\n";
        it = processFiles.emplace(pid, std::move(f)).first;
    }

    it->second << line << "\n"
               << std::flush;
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
    // 1 Clock tick
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

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
        writePrintRecord(process->id, coreId, outputString);
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
    return true;
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

            while (currentProc->programCounter < currentProc->instructions.size())
            {
                const auto &instr = currentProc->instructions[currentProc->programCounter];
                bool continueExec = executeInstruction(currentProc, coreId, instr, 0);

                currentProc->programCounter++;
                if (!continueExec) // process SLEEP
                {
                    yielded = true;
                    break;
                }
            }

            currentProc->isFinished = true;

            // Remove process from runningList
            {
                std::lock_guard<std::mutex> lock(runningMutex);
                runningList.erase(
                    std::remove(runningList.begin(), runningList.end(), currentProc),
                    runningList.end());
            }

            if (yielded)
            {
                // yield core and place process in sleepingList
                {
                    std::lock_guard<std::mutex> lock(sleepingMutex);
                    sleepingList.push_back(currentProc);
                }
            }
            else
            {
                // process completed
                currentProc->isFinished = true;
                {
                    std::lock_guard<std::mutex> lock(finishedMutex);
                    finishedList.push_back(currentProc);
                }
            }

            // Free assigned core
            {
                std::lock_guard<std::mutex> lock(coreMutex);
                coreAssignments[coreId] = nullptr;
            }
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
                for (int i = 0; i < NUM_CORES; ++i)
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

// NOTE TO GRPMATE DOING SCREEN COMMANDS (?): incomplete details to show pa, check specs nalang
std::string generateSystemReport()
{
    std::ostringstream oss;

    oss << "----------------------------------------\n";
    oss << "Running processes:\n";
    {
        std::lock_guard<std::mutex> lock(runningMutex);
        for (auto &p : runningList)
        {
            int done = p->totalCommands - p->remainingCommands;
            oss << std::left << std::setw(10) << p->id
                << "(" << p->submitTime << ")   "
                << std::setw(10) << ("Core: " + std::to_string(p->assignedCore.load()))
                << p->programCounter << " / " << p->instructions.size() << "\n";
        }
    }
    oss << "\nFinished processes:\n";
    {
        std::lock_guard<std::mutex> lock(finishedMutex);
        for (auto &p : finishedList)
        {
            oss << std::left << std::setw(10) << p->id
                << "(" << p->submitTime << ")   "
                << std::setw(10) << "Finished"
                << p->instructions.size() << " / " << p->instructions.size() << "\n";
        }
    }
    oss << "----------------------------------------\n ";

    return oss.str();
}

void cmdScreenLs()
{
    std::cout << generateSystemReport() << "> ";
}

void cmdReportUtil()
{
    std::string report = generateSystemReport();
    std::ofstream outFile("csopesy-log.txt", std::ios::out | std::ios::trunc);

    if (outFile.is_open())
    {
        outFile << report;
        outFile.close();
        std::cout << "Report generated at csopesy-log.txt!\n> ";
    }
    else
    {
        std::cout << "Failed to generate report file.\n> ";
    }
}

// "Type "exit" to close your emulator."
void shutdownEmulator()
{
    std::cout << "\nShutting down...\n";
    emulatorRunning = false;
    queueCV.notify_all(); // wake up sleeping workers to stop for shutting down
}

int main()
{
    // Create the 10 processes and push them to the ready queue
    for (int i = 1; i <= 10; ++i)
    {
        std::ostringstream name;
        name << "process" << std::setw(2) << std::setfill('0') << i;

        auto instrs = generateSampleInstructions(i); // NOTE TO GRPMATE: this must be called in scheduler-start. For now, i put it here
        auto proc = std::make_shared<Process>(name.str(), instrs);

        std::lock_guard<std::mutex> lock(queueMutex);
        readyQueue.push(proc);
    }

    std::vector<std::thread> workers; // start the worker threads
    for (int i = 0; i < NUM_CORES; ++i)
    {
        workers.emplace_back(workerThread, i);
    }

    std::thread scheduler(schedulerThread);

    std::cout << "> ";
    std::string input;
    while (emulatorRunning && std::getline(std::cin, input))
    {
        if (input == "screen -ls")
        {
            cmdScreenLs();
        }
        else if (input == "report-util")
        {
            cmdReportUtil();
        }
        else if (input == "exit")
        {
            shutdownEmulator();
            break;
        }
        else
        {
            std::cout << "Available commands: screen -ls | report-util | exit ";
        }
    }

    // wait for all threads to stop first
    emulatorRunning = false;
    queueCV.notify_all();
    for (auto &t : workers)
        if (t.joinable())
            t.join();
    if (scheduler.joinable())
        scheduler.join();

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
