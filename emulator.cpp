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

const int NUM_CORES = 4;

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

void writePrintRecord(const std::string &pid, int core, int cmdNum)
{
    std::string line = "(" + getTimestamp() + ") Core:" + std::to_string(core) + " \"Hello world from " + pid + "!\"";
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
            int cmdNum = 1;
            while (currentProc->remainingCommands > 0)
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
    // Create the 10 processes and push them to the ready queue
    for (int i = 1; i <= 10; ++i)
    {
        std::ostringstream name;
        name << "process" << std::setw(2) << std::setfill('0') << i;

        std::lock_guard<std::mutex> lock(queueMutex);
        readyQueue.push(std::make_shared<Process>(name.str(), 100));
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
