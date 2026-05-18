// ============================================================
//  Server (Raspberry Pi side) 132
//  Build: g++ -std=c++17 -O2 -pthread server.cpp -o server
// ============================================================
#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <memory>
#include <string>
#include <stdexcept>
#include <cstdint>
#include <algorithm>
#include <atomic>
#include <map>
#include <chrono>
#include <ctime>
#include <sstream>
#include <fstream>
#include <limits>
#include <iomanip>
#include <csignal>
#include <cstring>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <errno.h>
#include <termios.h>

// ============================================================
// ПРОТОКОЛ
// ============================================================
enum class MessageType : uint32_t {
    Text        = 1,
    Connect     = 2,
    Disconnect  = 3,
    LogRequest  = 4,
    LogResponse = 5,
    Warning     = 6,
    UartSend    = 7,
    UartData    = 8
};

#pragma pack(push, 1)
struct MessageHeader {
    uint32_t type;
    uint32_t size;
};
#pragma pack(pop)

constexpr uint32_t MAX_PAYLOAD  = 1u << 20;
constexpr uint32_t MAX_USERNAME = 64;

// ============================================================
// КОНФИГ
// ============================================================
constexpr int    PORT                 = 54002;
constexpr int    MONITOR_INTERVAL_SEC = 10;
constexpr double CPU_TEMP_LIMIT       = 70.0;
constexpr int    RAM_LIMIT_PERCENT    = 80;
constexpr const char* LOG_PATH        = "server.log";
constexpr int    TEST_COUNT           = 10;
constexpr int    TEST_INTERVAL_MS     = 500;

// ============================================================
// СОСТОЯНИЕ
// ============================================================
struct Client {
    int         socket;
    std::string color;
    int         colorIndex;
    std::string clientId;
    std::string username;
    std::string joinTime;
    std::shared_ptr<std::mutex> sendMutex;
};

static std::vector<Client>        g_clients;
static std::mutex                 g_clientsMutex;

static std::map<int, std::string> g_usedColors;
static std::mutex                 g_colorsMutex;

static int        g_uartFd = -1;
static std::mutex g_uartMutex;

static const std::vector<std::string> g_colorPool = {
    "\033[31m","\033[32m","\033[33m",
    "\033[34m","\033[35m","\033[36m"
};
static const std::string RESET = "\033[0m";

static std::atomic<bool> g_running{true};
static int               g_serverSock = -1;

static std::ofstream             g_logFile;
static std::vector<std::string>  g_logBuffer;
static std::vector<std::string>  g_warningBuffer;
static std::vector<std::string>  g_uartBuffer;   // буфер данных с Arduino
static std::mutex                g_logMutex;

// ============================================================
// UART
// ============================================================
static bool openUart(const char* device = "/dev/ttyUSB0", int baud = B9600) {
    g_uartFd = open(device, O_RDWR | O_NOCTTY | O_NDELAY);
    if (g_uartFd < 0) return false;
    termios opts{};
    tcgetattr(g_uartFd, &opts);
    cfsetispeed(&opts, baud);
    cfsetospeed(&opts, baud);
    opts.c_cflag |= (CLOCAL | CREAD);
    opts.c_cflag &= ~PARENB;
    opts.c_cflag &= ~CSTOPB;
    opts.c_cflag &= ~CSIZE;
    opts.c_cflag |= CS8;
    opts.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tcsetattr(g_uartFd, TCSANOW, &opts);
    return true;
}

// ============================================================
// УТИЛИТЫ ВРЕМЕНИ
// ============================================================
static std::string fmtTime(const char* fmt) {
    time_t now = time(nullptr);
    struct tm tmv{};
    localtime_r(&now, &tmv);
    char buf[64];
    strftime(buf, sizeof(buf), fmt, &tmv);
    return buf;
}
static std::string getSystemTimeFull() { return fmtTime("%Y-%m-%d %H:%M:%S"); }
static std::string getCurrentTime()    { return fmtTime("%H:%M:%S"); }

// ============================================================
// ЛОГИРОВАНИЕ
// ============================================================
static void logEvent(const std::string& text) {
    const std::string full = "[" + getSystemTimeFull() + "] " + text;
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile.is_open()) {
        g_logFile << full << '\n';
        g_logFile.flush();
    }
    g_logBuffer.push_back(full);
    if (text.find("[WARNING]") != std::string::npos)
        g_warningBuffer.push_back(full);
    if (g_logBuffer.size() > 50000)
        g_logBuffer.erase(g_logBuffer.begin(), g_logBuffer.begin() + 10000);
    if (g_warningBuffer.size() > 10000)
        g_warningBuffer.erase(g_warningBuffer.begin(), g_warningBuffer.begin() + 2000);
}

// ============================================================
// СЕТЕВЫЕ УТИЛИТЫ
// ============================================================
static void sendAllRaw(int sock, const char* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        ssize_t res = send(sock, data + sent, size - sent, MSG_NOSIGNAL);
        if (res <= 0) throw std::runtime_error("send failed");
        sent += static_cast<size_t>(res);
    }
}

static void sendFrame(int sock, std::mutex& m,
                      MessageType type, const std::string& payload)
{
    MessageHeader header{ (uint32_t)type, (uint32_t)payload.size() };
    std::lock_guard<std::mutex> lock(m);
    sendAllRaw(sock, (char*)&header, sizeof(header));
    if (!payload.empty())
        sendAllRaw(sock, payload.data(), payload.size());
}

static void recvAll(int sock, char* data, size_t size) {
    size_t got = 0;
    while (got < size) {
        ssize_t res = recv(sock, data + got, size - got, 0);
        if (res <= 0) throw std::runtime_error("recv failed");
        got += static_cast<size_t>(res);
    }
}

static std::string peerId(int sock) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getpeername(sock, (sockaddr*)&addr, &len) == 0) {
        char ipBuf[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &addr.sin_addr, ipBuf, sizeof(ipBuf));
        return std::string(ipBuf) + ":" + std::to_string(ntohs(addr.sin_port));
    }
    return "unknown";
}

// ============================================================
// ЦВЕТА
// ============================================================
static int acquireColor(const std::string& id) {
    std::lock_guard<std::mutex> lock(g_colorsMutex);
    for (int i = 0; i < (int)g_colorPool.size(); ++i) {
        if (!g_usedColors.count(i)) {
            g_usedColors[i] = id;
            return i;
        }
    }
    return -1;
}
static void releaseColor(int idx) {
    if (idx < 0) return;
    std::lock_guard<std::mutex> lock(g_colorsMutex);
    g_usedColors.erase(idx);
}

// ============================================================
// BROADCAST
// ============================================================
static void broadcast(MessageType type, const std::string& payload, int senderSock) {
    std::vector<std::pair<int, std::shared_ptr<std::mutex>>> targets;
    {
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        targets.reserve(g_clients.size());
        for (auto& c : g_clients)
            if (c.socket != senderSock)
                targets.emplace_back(c.socket, c.sendMutex);
    }
    for (auto& t : targets) {
        try { sendFrame(t.first, *t.second, type, payload); }
        catch (...) {}
    }
}

// ============================================================
// UART READ LOOP — только сохраняет, не рассылает
// ============================================================
static void uartReadLoop() {
    char buf[256];
    std::string partial;
    while (g_running && g_uartFd >= 0) {
        int n = read(g_uartFd, buf, sizeof(buf));
        if (n > 0) {
            partial.append(buf, n);
            size_t pos;
            while ((pos = partial.find('\n')) != std::string::npos) {
                std::string line = partial.substr(0, pos);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (!line.empty()) {
                    std::lock_guard<std::mutex> lock(g_logMutex);
                    std::string entry = "[" + getSystemTimeFull() + "] " + line;
                    g_uartBuffer.push_back(entry);
                    if (g_uartBuffer.size() > 10000)
                        g_uartBuffer.erase(g_uartBuffer.begin(),
                                           g_uartBuffer.begin() + 2000);
                }
                partial = partial.substr(pos + 1);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

// ============================================================
// ЧТЕНИЕ /proc И /sys
// ============================================================
static std::string getUptime() {
    std::ifstream f("/proc/uptime");
    double seconds = 0;
    if (!(f >> seconds)) return "Unknown";
    int hrs  =  (int)seconds / 3600;
    int mins = ((int)seconds % 3600) / 60;
    int secs =  (int)seconds % 60;
    std::ostringstream oss;
    oss << hrs << "h " << mins << "m " << secs << "s";
    return oss.str();
}

static int getRAMUsagePercent() {
    std::ifstream f("/proc/meminfo");
    std::string key;
    long total = 0, available = 0;
    while (f >> key) {
        if (key == "MemTotal:")          f >> total;
        else if (key == "MemAvailable:") { f >> available; break; }
        f.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    if (total <= 0) return 0;
    return (int)(((total - available) * 100) / total);
}

static double getCPUTempValue() {
    std::ifstream f("/sys/class/thermal/thermal_zone0/temp");
    if (!f.is_open()) return -1;
    long raw = 0;
    if (!(f >> raw)) return -1;
    return raw / 1000.0;
}

static std::string formatTemp(double t) {
    if (t < 0) return "n/a";
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << t << " C";
    return oss.str();
}

static std::string buildStatusString() {
    double temp = getCPUTempValue();
    int    ram  = getRAMUsagePercent();
    auto   up   = getUptime();
    std::ostringstream oss;
    oss << "=== Raspberry status (" << getSystemTimeFull() << ") ===\n"
        << "  CPU temperature : " << formatTemp(temp) << "\n"
        << "  RAM usage       : " << ram  << " %\n"
        << "  Uptime          : " << up   << "\n"
        << "  Clients online  : ";
    {
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        oss << g_clients.size() << "\n";
        for (auto& c : g_clients)
            oss << "    - " << c.username << " (" << c.clientId
                << ", joined " << c.joinTime << ")\n";
    }
    return oss.str();
}

// ============================================================
// ФОНОВЫЙ МОНИТОР
// ============================================================
static void monitoringLoop() {
    while (g_running) {
        double cpuTemp = getCPUTempValue();
        int    ram     = getRAMUsagePercent();
        auto   up      = getUptime();

        std::ostringstream line;
        line << "MONITOR | CPU: " << formatTemp(cpuTemp)
             << " | RAM: " << ram << "% | Uptime: " << up;
        logEvent(line.str());

        std::string warn;
        if (cpuTemp > 0 && cpuTemp > CPU_TEMP_LIMIT)
            warn += "CPU TEMP HIGH (" + formatTemp(cpuTemp) + ") ";
        if (ram > RAM_LIMIT_PERCENT)
            warn += "RAM USAGE HIGH (" + std::to_string(ram) + "%)";

        if (!warn.empty()) {
            std::string full = "[WARNING] " + warn;
            std::cout << "\033[31m" << full << RESET << "\n";
            logEvent(full);
            broadcast(MessageType::Warning, full, -1);
        }

        for (int i = 0; i < MONITOR_INTERVAL_SEC * 10 && g_running; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// ============================================================
// TEST
// ============================================================
static void runTestFlood(int sock, std::shared_ptr<std::mutex> sendMutex,
                         std::string username)
{
    try {
        for (int i = 1; i <= TEST_COUNT && g_running; ++i) {
            std::ostringstream oss;
            oss << "TEST " << i << "/" << TEST_COUNT;
            sendFrame(sock, *sendMutex, MessageType::Text, oss.str());
            std::this_thread::sleep_for(std::chrono::milliseconds(TEST_INTERVAL_MS));
        }
        sendFrame(sock, *sendMutex, MessageType::LogResponse,
                  "Test flood finished (" + std::to_string(TEST_COUNT) + " messages).\n");
    } catch (...) {}
    logEvent("Test flood for '" + username + "' done.");
}

// ============================================================
// LOG REQUEST HANDLER
// ============================================================
static std::string handleLogRequest(const std::string& request) {
    if (request == "TEMP") {
        return "CPU temperature: " + formatTemp(getCPUTempValue()) + "\n";
    }
    if (request == "STATUS") {
        return buildStatusString();
    }
    if (request == "ALL") {
        std::lock_guard<std::mutex> lock(g_logMutex);
        std::ostringstream oss;
        for (auto& l : g_logBuffer) oss << l << '\n';
        auto s = oss.str();
        return s.empty() ? "No logs yet.\n" : s;
    }
    if (request == "WARNINGS") {
        std::lock_guard<std::mutex> lock(g_logMutex);
        std::ostringstream oss;
        for (auto& l : g_warningBuffer) oss << l << '\n';
        auto s = oss.str();
        return s.empty() ? "No warnings recorded.\n" : s;
    }
    if (request.rfind("LAST ", 0) == 0) {
        int minutes = 0;
        try { minutes = std::stoi(request.substr(5)); }
        catch (...) { return "Bad LAST argument.\n"; }
        if (minutes <= 0) return "Minutes must be > 0.\n";
        time_t now = time(nullptr);
        std::ostringstream oss;
        std::lock_guard<std::mutex> lock(g_logMutex);
        for (auto& l : g_logBuffer) {
            if (l.size() < 21) continue;
            std::tm tm{};
            std::istringstream ss(l.substr(1, 19));
            ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
            if (ss.fail()) continue;
            tm.tm_isdst = -1;
            if (difftime(now, mktime(&tm)) <= minutes * 60)
                oss << l << '\n';
        }
        auto s = oss.str();
        return s.empty() ? "No logs in the requested interval.\n" : s;
    }
    // --- SENSOR команды ---
    if (request == "SENSOR_ALL") {
        std::lock_guard<std::mutex> lock(g_logMutex);
        std::ostringstream oss;
        for (auto& l : g_uartBuffer) oss << l << '\n';
        auto s = oss.str();
        return s.empty() ? "No sensor data yet.\n" : s;
    }
    if (request.rfind("SENSOR_LAST ", 0) == 0) {
        int minutes = 0;
        try { minutes = std::stoi(request.substr(12)); }
        catch (...) { return "Bad argument.\n"; }
        if (minutes <= 0) return "Minutes must be > 0.\n";
        time_t now = time(nullptr);
        std::ostringstream oss;
        std::lock_guard<std::mutex> lock(g_logMutex);
        for (auto& l : g_uartBuffer) {
            if (l.size() < 21) continue;
            std::tm tm{};
            std::istringstream ss(l.substr(1, 19));
            ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
            if (ss.fail()) continue;
            tm.tm_isdst = -1;
            if (difftime(now, mktime(&tm)) <= minutes * 60)
                oss << l << '\n';
        }
        auto s = oss.str();
        return s.empty() ? "No sensor data in this interval.\n" : s;
    }
    return "Unknown log request.\n";
}

// ============================================================
// КЛИЕНТСКИЙ ПОТОК
// ============================================================
static void handleClient(int clientSocket, int colorIdx, std::string clientColor,
                         std::shared_ptr<std::mutex> sendMutex)
{
    std::string clientId = peerId(clientSocket);
    std::string username = "Unknown";

    auto cleanup = [&](){
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        g_clients.erase(std::remove_if(g_clients.begin(), g_clients.end(),
            [clientSocket](const Client& c){ return c.socket == clientSocket; }),
            g_clients.end());
    };

    try {
        MessageHeader header{};
        recvAll(clientSocket, (char*)&header, sizeof(header));

        if (header.type != (uint32_t)MessageType::Connect ||
            header.size == 0 || header.size > MAX_USERNAME)
        {
            close(clientSocket);
            releaseColor(colorIdx);
            cleanup();
            return;
        }

        std::vector<char> data(header.size);
        recvAll(clientSocket, data.data(), header.size);
        username.assign(data.begin(), data.end());

        std::string joinTime = getCurrentTime();
        {
            std::lock_guard<std::mutex> lock(g_clientsMutex);
            for (auto& c : g_clients)
                if (c.socket == clientSocket) {
                    c.username = username;
                    c.joinTime = joinTime;
                }
        }

        std::cout << clientColor << "Client " << clientId
                  << " joined as '" << username << "'" << RESET << "\n";
        logEvent("Client " + clientId + " joined as '" + username + "'");
        broadcast(MessageType::Text,
                  "*** " + username + " joined the chat ***", clientSocket);

        while (g_running) {
            recvAll(clientSocket, (char*)&header, sizeof(header));
            if (header.size > MAX_PAYLOAD) break;

            std::vector<char> msg(header.size);
            if (header.size > 0) recvAll(clientSocket, msg.data(), header.size);

            switch ((MessageType)header.type) {
                case MessageType::Text: {
                    std::string text(msg.begin(), msg.end());
                    std::cout << clientColor << "[" << username << "] "
                              << text << RESET << "\n";
                    logEvent("[" + username + "] " + text);
                    broadcast(MessageType::Text,
                              clientColor + text + RESET, clientSocket);
                    break;
                }
                case MessageType::LogRequest: {
                    std::string req(msg.begin(), msg.end());
                    if (req == "TEST") {
                        logEvent("Test flood started for '" + username + "'");
                        std::thread(runTestFlood, clientSocket,
                                    sendMutex, username).detach();
                    } else {
                        std::string out = handleLogRequest(req);
                        sendFrame(clientSocket, *sendMutex,
                                  MessageType::LogResponse, out);
                    }
                    break;
                }
                case MessageType::UartSend: {
                    std::string cmd(msg.begin(), msg.end());
                    logEvent("[UART→] from " + username + ": " + cmd);
                    std::lock_guard<std::mutex> ul(g_uartMutex);
                    if (g_uartFd >= 0)
                        write(g_uartFd, cmd.data(), cmd.size());
                    break;
                }
                case MessageType::Disconnect:
                    goto done;
                default:
                    break;
            }
        }
    }
    catch (...) {}

done:
    std::cout << "Client " << clientId << " (" << username << ") disconnected.\n";
    logEvent("Client " + clientId + " (" + username + ") disconnected.");
    broadcast(MessageType::Text,
              "*** " + username + " left the chat ***", clientSocket);
    cleanup();
    releaseColor(colorIdx);
    close(clientSocket);
}

// ============================================================
// ACCEPT
// ============================================================
static void runAcceptLoop() {
    fcntl(g_serverSock, F_SETFL, O_NONBLOCK);
    while (g_running) {
        sockaddr_in addr{};
        socklen_t   len = sizeof(addr);
        int client = accept(g_serverSock, (sockaddr*)&addr, &len);
        if (client < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (!g_running) break;
            std::cerr << "accept() error: " << strerror(errno) << "\n";
            break;
        }

        std::string id    = peerId(client);
        int colorIdx      = acquireColor(id);
        std::string color = (colorIdx >= 0) ? g_colorPool[colorIdx] : "\033[37m";
        auto sendMutex    = std::make_shared<std::mutex>();

        Client nc;
        nc.socket     = client;
        nc.clientId   = id;
        nc.username   = "Pending...";
        nc.colorIndex = colorIdx;
        nc.color      = color;
        nc.joinTime   = getCurrentTime();
        nc.sendMutex  = sendMutex;

        {
            std::lock_guard<std::mutex> lock(g_clientsMutex);
            g_clients.push_back(nc);
        }
        std::cout << "New connection from " << id << "\n";
        logEvent("New connection from " + id);
        std::thread(handleClient, client, colorIdx, color, sendMutex).detach();
    }
}

// ============================================================
// СИГНАЛЫ
// ============================================================
static void onSignal(int) {
    g_running = false;
    if (g_serverSock >= 0) {
        ::shutdown(g_serverSock, SHUT_RDWR);
        ::close(g_serverSock);
        g_serverSock = -1;
    }
}

// ============================================================
// MAIN
// ============================================================
int main() {
    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);

    g_logFile.open(LOG_PATH, std::ios::app);
    logEvent("=== Server started ===");

    if (!openUart("/dev/ttyUSB0", B9600))
        std::cerr << "Warning: UART not available\n";
    else
        std::thread(uartReadLoop).detach();

    g_serverSock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_serverSock < 0) { std::cerr << "socket() failed\n"; return 1; }
    int yes = 1;
    setsockopt(g_serverSock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(g_serverSock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "bind() failed: " << strerror(errno) << "\n"; return 1;
    }
    if (listen(g_serverSock, SOMAXCONN) < 0) {
        std::cerr << "listen() failed: " << strerror(errno) << "\n"; return 1;
    }

    std::cout << "Server running on port " << PORT << " ...\n"
              << "Monitoring every " << MONITOR_INTERVAL_SEC << "s "
              << "(CPU>" << CPU_TEMP_LIMIT << "C, RAM>"
              << RAM_LIMIT_PERCENT << "% -> WARNING)\n"
              << "Press Ctrl+C to stop.\n";

    std::thread monitor(monitoringLoop);
    runAcceptLoop();

    if (monitor.joinable()) monitor.join();

    logEvent("=== Server stopped ===");
    if (g_logFile.is_open()) g_logFile.close();
    if (g_serverSock >= 0) close(g_serverSock);
    return 0;
}