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

#ifndef ENABLE_PRINT_FILES_FLAG
#define ENABLE_PRINT_FILES_FLAG 1 // 1 = homework, 0 = machine project
#endif

const bool ENABLE_PRINT_FILES = ENABLE_PRINT_FILES_FLAG;

// TO DO: "Declare 4 cores for the CPU scheduler."
const int NUM_CORES = 4;

// TO DO: "Create 10 processes, each with 100 print commands, upon the start of your OS emulator."

// timestamp used by Process and the print records, ex: 08/06/2024 09:15:22AM
std::string getTimestamp()
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

// Process struct (name, submit time, total commands, remaining commands, assigned core, finished flag)
struct Process
{
    std::string id;
    std::string submitTime;
    int totalCommands;
    std::atomic<int> remainingCommands;
    std::atomic<int> assignedCore{-1}; // -1 = not on a core yet
    std::atomic<bool> isFinished{false};

    Process(std::string pid, int cmds)
        : id(pid), totalCommands(cmds), remainingCommands(cmds)
    {
        submitTime = getTimestamp(); // time the process was created
    }
};

std::queue<std::shared_ptr<Process>> readyQueue; // FCFS
std::mutex queueMutex;

std::vector<std::shared_ptr<Process>> runningList; // processes on a core right now
std::mutex runningMutex;

std::vector<std::shared_ptr<Process>> finishedList; // processes that are done
std::mutex finishedMutex;

std::condition_variable queueCV;                   // wakes up the workers; will be used when exiting
std::map<std::string, std::ofstream> processFiles; // one file per process
std::mutex fileMutex;
std::atomic<bool> emulatorRunning{true}; // set to false on exit

// Core assignments state to manage exactly 4 worker threads
std::vector<std::shared_ptr<Process>> coreAssignments(NUM_CORES, nullptr);
std::mutex coreMutex;

// TO DO: "Implement the "print" command with the following behavior: each process creates a text file
// where all its associated print commands are written, with the timestamp of when it was executed by
// the CPU and the CPU core that executed it."
// TO DO: "Thus, if there are 10 processes, 10 text files should be created."
void writePrintRecord(const std::string &pid, int core, int cmdNum)
{
    if (!ENABLE_PRINT_FILES)
        return; // files disabled for the machine project

    // TO DO: open <pid>_output.txt and write the timestamp, core and command number
}

// TO DO: "implement a first-come-first-serve (FCFS) scheduler, emphasizing the multi-threaded approach
// (e.g., 1 thread for the scheduler, 1 thread per CPU worker)"
void workerThread(int coreId)
{
    // TO DO: 1 thread per CPU worker
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
            int cmdNum = 1;
            while (emulatorRunning && currentProc->remainingCommands > 0)
            {
                writePrintRecord(currentProc->id, coreId, cmdNum++);
                currentProc->remainingCommands--;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            currentProc->isFinished = true;

            // Remove process from runningList
            {
                std::lock_guard<std::mutex> lock(runningMutex);
                for (auto it = runningList.begin(); it != runningList.end(); ++it)
                {
                    if ((*it)->id == currentProc->id)
                    {
                        runningList.erase(it);
                        break;
                    }
                }
            }

            // Move process to finishedList
            {
                std::lock_guard<std::mutex> lock(finishedMutex);
                finishedList.push_back(currentProc);
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
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

// TO DO: "Periodically type the "screen -ls" command every 1 - 2 seconds until all processes are shown
// in the "finished processes" list."
void cmdScreenLs()
{
    std::cout << "----------------------------------------\n";
    std::cout << "Running processes:\n";
    {
        std::lock_guard<std::mutex> lock(runningMutex);
        for (auto &p : runningList)
        {
            int done = p->totalCommands - p->remainingCommands;
            std::cout << std::left << std::setw(10) << p->id
                      << "(" << p->submitTime << ")   "
                      << std::setw(10) << ("Core: " + std::to_string(p->assignedCore.load()))
                      << done << " / " << p->totalCommands << "\n";
        }
    }
    std::cout << "\nFinished processes:\n";
    {
        std::lock_guard<std::mutex> lock(finishedMutex);
        for (auto &p : finishedList)
        {
            std::cout << std::left << std::setw(10) << p->id
                      << "(" << p->submitTime << ")   "
                      << std::setw(10) << "Finished"
                      << p->totalCommands << " / " << p->totalCommands << "\n";
        }
    }
    std::cout << "----------------------------------------\n> ";
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
    std::vector<std::thread> workers; // TO DO: start the worker threads here
    for (int i = 0; i < NUM_CORES; ++i)
    {
        workers.emplace_back(workerThread, i);
    }

    std::thread scheduler(schedulerThread);

    // TO DO: create the 10 processes and push them to the ready queue

    std::cout << "> ";
    std::string input;
    while (emulatorRunning && std::getline(std::cin, input))
    {
        if (input == "screen -ls")
        {
            cmdScreenLs();
        }
        else if (input == "exit")
        {
            shutdownEmulator();
            break;
        }
        else
        {
            std::cout << "Available commands: screen -ls | exit\n> ";
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