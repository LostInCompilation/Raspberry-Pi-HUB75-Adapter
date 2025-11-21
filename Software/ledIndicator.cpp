/*
 * Bi-color LED System Activity Monitor for Raspberry Pi
 * Direct GPIO control for maximum responsiveness
 * Red = idle, Green flashes = CPU activity
 *
 * Compilation with optimizations:
 *   g++ -o led_monitor led_monitor.cpp -lpigpio -lrt -lpthread -O3 -march=native
 *
 * Run (requires sudo for direct GPIO access):
 *   sudo ./led_monitor
 *
 * Note: pigpiod daemon must NOT be running for direct GPIO access
 *   sudo systemctl stop pigpiod
 */

#include <pigpio.h>
#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
#include <random>
#include <csignal>
#include <cmath>
#include <sys/resource.h>

// ============================================================================
// CONFIGURATION - Adjust these settings to your preference
// ============================================================================

// GPIO pin configuration
const int PIN_A = 16;  // First LED pin
const int PIN_B = 26;  // Second LED pin

// Activity monitoring settings (optimized for responsiveness)
const int CHECK_INTERVAL_MS = 25;      // Check every 25ms (fast response)
const int MIN_FLASH_DURATION_MS = 12;  // Minimum flash duration at low CPU (milliseconds)
const int MAX_FLASH_DURATION_MS = 50;  // Maximum flash duration at high CPU (milliseconds)
const double ACTIVITY_THRESHOLD = 0.5; // Minimum CPU load to trigger flashes
const double ACTIVITY_SMOOTHING = 0.5; // Activity smoothing (lower = more responsive)

// LED brightness settings
const int GREEN_BRIGHTNESS = 32;       // Green brightness (0-255)
const int PWM_FREQUENCY = 1000;        // PWM frequency in Hz

// Flash probability settings
const double BASE_FLASH_CHANCE = 0.25; // Base probability multiplier
const double CPU_SCALING = 0.04;       // CPU influence on flash chance
const double FLASH_VARIATION = 0.3;    // Random variation in flash timing

// Minimum time between flashes
const int MIN_PAUSE_BETWEEN_FLASHES_MS = 30;

// Background mode (disable console output for lower CPU usage)
const bool BACKGROUND_MODE = false;  // Set to true when running as service

// ============================================================================

// Global flag for clean shutdown
volatile bool running = true;

// CPU stats structure
struct CPUStats {
    unsigned long long user;
    unsigned long long nice;
    unsigned long long system;
    unsigned long long idle;
    unsigned long long iowait;
    unsigned long long irq;
    unsigned long long softirq;
};

// LED control functions (RED = idle, GREEN = activity with PWM)
inline void setRed() {
    gpioPWM(PIN_B, 0);
    gpioWrite(PIN_A, 1);
    gpioWrite(PIN_B, 0);
}

inline void setGreen() {
    gpioWrite(PIN_A, 0);
    gpioPWM(PIN_B, GREEN_BRIGHTNESS);
}

inline void setOff() {
    gpioWrite(PIN_A, 0);
    gpioPWM(PIN_B, 0);
}

// Signal handler for clean shutdown
void signalHandler(int signum) {
    if (!BACKGROUND_MODE) {
        std::cout << "\n\nReceived signal " << signum << ", shutting down..." << std::endl;
    }
    running = false;
    setOff();
    gpioTerminate();
    exit(0);
}

// Read CPU stats from /proc/stat (optimized)
bool readCPUStats(CPUStats& stats) {
    std::ifstream statFile("/proc/stat");
    if (!statFile.is_open()) {
        return false;
    }

    std::string cpu;
    statFile >> cpu >> stats.user >> stats.nice >> stats.system >> stats.idle
             >> stats.iowait >> stats.irq >> stats.softirq;

    return true;
}

// CPU monitor class (optimized for minimal allocations)
class CPUMonitor {
private:
    CPUStats lastStats;
    double cpuLoad;
    bool initialized;

public:
    CPUMonitor() : cpuLoad(0.0), initialized(false) {
        readCPUStats(lastStats);
    }

    double getCPULoad() {
        CPUStats currentStats;
        if (!readCPUStats(currentStats)) {
            return cpuLoad;
        }

        if (!initialized) {
            initialized = true;
            lastStats = currentStats;
            return 0.0;
        }

        // Calculate deltas
        unsigned long long last_total = lastStats.user + lastStats.nice + lastStats.system +
                                        lastStats.idle + lastStats.iowait + lastStats.irq + lastStats.softirq;
        unsigned long long curr_total = currentStats.user + currentStats.nice + currentStats.system +
                                        currentStats.idle + currentStats.iowait + currentStats.irq + currentStats.softirq;

        unsigned long long total_diff = curr_total - last_total;
        unsigned long long idle_diff = (currentStats.idle + currentStats.iowait) -
                                       (lastStats.idle + lastStats.iowait);

        lastStats = currentStats;

        if (total_diff == 0) {
            return cpuLoad;
        }

        double currentLoad = 100.0 * (1.0 - (double)idle_diff / total_diff);
        cpuLoad = cpuLoad * ACTIVITY_SMOOTHING + currentLoad * (1.0 - ACTIVITY_SMOOTHING);

        return cpuLoad;
    }
};

int main() {
    // Set low priority for background operation
    setpriority(PRIO_PROCESS, 0, 19);

    // Initialize pigpio for direct GPIO access
    // Disable signal handling so our handlers work
    gpioCfgSetInternals(gpioCfgGetInternals() | PI_CFG_NOSIGHANDLER);

    if (gpioInitialise() < 0) {
        std::cerr << "ERROR: pigpio initialization failed!" << std::endl;
        std::cerr << "Make sure:" << std::endl;
        std::cerr << "  1. You're running with sudo" << std::endl;
        std::cerr << "  2. pigpiod daemon is NOT running (sudo killall pigpiod)" << std::endl;
        return 1;
    }

    // Setup signal handlers
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGSEGV, signalHandler);
    signal(SIGABRT, signalHandler);
    signal(SIGHUP, signalHandler);

    // Setup GPIO pins
    gpioSetMode(PIN_A, PI_OUTPUT);
    gpioSetMode(PIN_B, PI_OUTPUT);
    gpioSetPWMfrequency(PIN_B, PWM_FREQUENCY);
    gpioSetPWMrange(PIN_B, 255);

    if (!BACKGROUND_MODE) {
        std::cout << "System Activity Monitor Started (Direct GPIO)" << std::endl;
        std::cout << "LED pins: GPIO " << PIN_A << " and GPIO " << PIN_B << std::endl;
        std::cout << "Red = idle, Green flickers = CPU activity" << std::endl;
        std::cout << "Running with low priority (nice 19)" << std::endl;
        std::cout << "Press Ctrl+C to exit\n" << std::endl;
    }

    CPUMonitor monitor;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis(0.0, 1.0);

    setRed();
    bool isGreen = false;
    auto flashTimer = std::chrono::steady_clock::now();
    auto lastFlashEndTime = std::chrono::steady_clock::now();
    int currentFlashDuration = MIN_FLASH_DURATION_MS;

    while (running) {
        double cpuLoad = monitor.getCPULoad();
        auto currentTime = std::chrono::steady_clock::now();

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            currentTime - flashTimer).count();
        auto timeSinceLastFlash = std::chrono::duration_cast<std::chrono::milliseconds>(
            currentTime - lastFlashEndTime).count();

        if (isGreen && elapsed > currentFlashDuration) {
            setRed();
            isGreen = false;
            lastFlashEndTime = currentTime;
        } else if (!isGreen && cpuLoad > ACTIVITY_THRESHOLD && timeSinceLastFlash > MIN_PAUSE_BETWEEN_FLASHES_MS) {
            double randomFactor = 0.5 + dis(gen) * FLASH_VARIATION;
            double flashProbability = BASE_FLASH_CHANCE * (cpuLoad * CPU_SCALING) * randomFactor;

            if (dis(gen) < flashProbability) {
                double cpuFactor = std::min(1.0, cpuLoad / 100.0);
                int durationRange = MAX_FLASH_DURATION_MS - MIN_FLASH_DURATION_MS;
                currentFlashDuration = MIN_FLASH_DURATION_MS + (int)(durationRange * cpuFactor);

                int randomVariation = (int)(durationRange * 0.3 * dis(gen));
                currentFlashDuration += randomVariation - (randomVariation / 2);
                currentFlashDuration = std::max(MIN_FLASH_DURATION_MS,
                                               std::min(MAX_FLASH_DURATION_MS, currentFlashDuration));

                setGreen();
                isGreen = true;
                flashTimer = currentTime;
            }
        }

        // Only show output if not in background mode
        if (!BACKGROUND_MODE) {
            int barLength = std::min(50, (int)(cpuLoad / 2));
            printf("\rCPU: %5.1f%% %s [%.*s%.*s]",
                   cpuLoad,
                   isGreen ? "*" : " ",
                   barLength, "##################################################",
                   50 - barLength, "--------------------------------------------------");
            fflush(stdout);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(CHECK_INTERVAL_MS));
    }

    setOff();
    gpioTerminate();

    if (!BACKGROUND_MODE) {
        std::cout << std::endl;
    }

    return 0;
}
