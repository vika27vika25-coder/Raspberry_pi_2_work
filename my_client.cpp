// Client 132
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS

#include <WinSock2.h>
#include <Ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <sstream>
#include <stdexcept>
#include <cstdint>
#include <ctime>

#pragma comment(lib, "Ws2_32.lib")

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

constexpr uint32_t MAX_PAYLOAD = 1u << 20;

// ============================================================
// СОСТОЯНИЕ
// ============================================================
static SOCKET             g_socket = INVALID_SOCKET;
static std::atomic<bool>  g_connected{false};
static std::atomic<bool>  g_running  {true};
static std::mutex         g_coutMutex;
static std::string        g_username;

// ============================================================
// КОНСОЛЬ И UTF-8
// ============================================================
static void setupConsole() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode))
            SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

// ============================================================
// WINSOCK
// ============================================================
static void initWinSock()    { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); }
static void cleanupWinSock() { WSACleanup(); }

// ============================================================
// УТИЛИТЫ
// ============================================================
static std::string getCurrentTime() {
    time_t now = time(nullptr);
    struct tm ti;
    localtime_s(&ti, &now);
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", &ti);
    return buf;
}

// ============================================================
// СЕТЬ
// ============================================================
static void sendAll(const char* data, int size) {
    int sent = 0;
    while (sent < size) {
        int r = send(g_socket, data + sent, size - sent, 0);
        if (r <= 0) throw std::runtime_error("send failed");
        sent += r;
    }
}

static void recvAll(char* data, int size) {
    int got = 0;
    while (got < size) {
        int r = recv(g_socket, data + got, size - got, 0);
        if (r <= 0) throw std::runtime_error("recv failed");
        got += r;
    }
}

// ============================================================
// ОТПРАВКА
// ============================================================
static void sendChat(const std::string& input) {
    std::string body = "[" + g_username + " " + getCurrentTime() + "] " + input;
    MessageHeader header{ (uint32_t)MessageType::Text, (uint32_t)body.size() };
    sendAll((char*)&header, sizeof(header));
    sendAll(body.data(), (int)body.size());
}

static void sendLogRequest(const std::string& payload) {
    MessageHeader header{ (uint32_t)MessageType::LogRequest,
                          (uint32_t)payload.size() };
    sendAll((char*)&header, sizeof(header));
    if (!payload.empty()) sendAll(payload.data(), (int)payload.size());
}

static void sendUartCommand(const std::string& cmd) {
    MessageHeader header{ (uint32_t)MessageType::UartSend, (uint32_t)cmd.size() };
    sendAll((char*)&header, sizeof(header));
    sendAll(cmd.data(), (int)cmd.size());
}

// ============================================================
// ВВОД ИМЕНИ
// ============================================================
static std::string askUsername() {
    while (true) {
        std::cout << "Enter your name (3-20 characters): ";
        std::string name;
        std::getline(std::cin, name);
        size_t first = name.find_first_not_of(" \t");
        if (first == std::string::npos) { std::cout << "Name cannot be empty.\n"; continue; }
        size_t last = name.find_last_not_of(" \t");
        name = name.substr(first, last - first + 1);
        if (name.length() < 3 || name.length() > 20) {
            std::cout << "Length must be between 3 and 20.\n"; continue;
        }
        return name;
    }
}

// ============================================================
// ПОТОК ПРИЁМА
// ============================================================
static void printPrompt() { std::cout << "> " << std::flush; }

static void receiveLoop() {
    try {
        while (g_connected) {
            MessageHeader header{};
            recvAll((char*)&header, sizeof(header));
            if (header.size > MAX_PAYLOAD)
                throw std::runtime_error("payload too big");

            std::vector<char> data(header.size);
            if (header.size > 0) recvAll(data.data(), (int)header.size);
            std::string text(data.begin(), data.end());

            std::lock_guard<std::mutex> lock(g_coutMutex);
            switch ((MessageType)header.type) {
                case MessageType::LogResponse:
                    std::cout << "\n--- response ---\n"
                              << text
                              << "----------------\n";
                    break;
                case MessageType::Warning:
                    std::cout << "\n\033[31m[!! SERVER WARNING] "
                              << text << "\033[0m\n";
                    break;
                case MessageType::UartData:
                    std::cout << "\n[SENSOR] " << text << "\n";
                    break;
                case MessageType::Text:
                    std::cout << "\n" << text << "\n";
                    break;
                default:
                    std::cout << "\n" << text << "\n";
                    break;
            }
            printPrompt();
        }
    }
    catch (...) {
        std::lock_guard<std::mutex> lock(g_coutMutex);
        std::cout << "\n[disconnected from server]\n";
        printPrompt();
        g_connected = false;
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
    }
}

// ============================================================
// CONNECT / DISCONNECT
// ============================================================
static bool connectToServer(const std::string& ip, int port) {
    g_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_socket == INVALID_SOCKET) { std::cout << "socket() failed\n"; return false; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((u_short)port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        std::cout << "Invalid IP address.\n";
        closesocket(g_socket); g_socket = INVALID_SOCKET; return false;
    }
    if (connect(g_socket, (sockaddr*)&addr, sizeof(addr)) != 0) {
        std::cout << "Connection failed (code " << WSAGetLastError() << ").\n";
        closesocket(g_socket); g_socket = INVALID_SOCKET; return false;
    }
    try {
        MessageHeader header{ (uint32_t)MessageType::Connect,
                              (uint32_t)g_username.size() };
        sendAll((char*)&header, sizeof(header));
        sendAll(g_username.data(), (int)g_username.size());
    }
    catch (...) {
        std::cout << "Failed to send hello.\n";
        closesocket(g_socket); g_socket = INVALID_SOCKET; return false;
    }
    g_connected = true;
    std::thread(receiveLoop).detach();
    std::cout << "Connected to " << ip << ":" << port
              << " as '" << g_username << "'\n";
    return true;
}

static void disconnectFromServer() {
    if (!g_connected) return;
    try {
        MessageHeader header{ (uint32_t)MessageType::Disconnect,
                              (uint32_t)g_username.size() };
        sendAll((char*)&header, sizeof(header));
        sendAll(g_username.data(), (int)g_username.size());
    } catch (...) {}
    g_connected = false;
    if (g_socket != INVALID_SOCKET) {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
    }
    std::cout << "Disconnected.\n";
}

// ============================================================
// НОРМАЛИЗАЦИЯ КОМАНД
// ============================================================
static std::string normalizeCommand(const std::string& input, std::string& rest) {
    if (input.empty() || input[0] != '/') return "";

    size_t sp = input.find(' ');
    std::string head = (sp == std::string::npos) ? input : input.substr(0, sp);
    rest             = (sp == std::string::npos) ? "" : input.substr(sp + 1);

    for (auto& ch : head) ch = (char)tolower((unsigned char)ch);

    static const std::vector<std::pair<std::string, std::string>> table = {
        {"/connect",     "/connect"},    {"/c",  "/connect"},
        {"/disconnect",  "/disconnect"}, {"/d",  "/disconnect"},
        {"/exit",        "/exit"},       {"/q",  "/exit"}, {"/quit", "/exit"},
        {"/help",        "/help"},       {"/h",  "/help"}, {"/?",    "/help"},
        {"/temp",        "/temp"},       {"/t",  "/temp"},
        {"/status",      "/status"},     {"/s",  "/status"},
        {"/test",        "/test"},
        {"/logs",        "/logs"},       {"/l",  "/logs"},
        {"/uart",        "/uart"},       {"/u",  "/uart"},
        {"/sensor_all",  "/sensor_all"},
        {"/sensor_last", "/sensor_last"},
    };
    for (auto& p : table)
        if (head == p.first) return p.second;

    return head;
}

// ============================================================
// LOGS PAYLOAD
// ============================================================
static std::string buildLogsPayload(const std::string& rest) {
    if (rest.empty()) return "";
    std::stringstream ss(rest);
    std::string sub;
    ss >> sub;
    for (auto& ch : sub) ch = (char)tolower((unsigned char)ch);

    if (sub == "all" || sub == "a")                    return "ALL";
    if (sub == "warnings" || sub == "warn" || sub == "w") return "WARNINGS";
    if (sub == "last" || sub == "l") {
        int minutes = 0;
        if (!(ss >> minutes) || minutes <= 0) return "";
        return "LAST " + std::to_string(minutes);
    }
    return "";
}

// ============================================================
// HELP
// ============================================================
static void printHelp() {
    std::cout <<
        "\nCommands (short aliases in parentheses):\n"
        "  /connect <ip> <port>      (/c)     connect to the Raspberry server\n"
        "  /disconnect               (/d)     close current connection\n"
        "  /temp                     (/t)     current CPU temperature\n"
        "  /status                   (/s)     full status (CPU/RAM/uptime/clients)\n"
        "  /test                              ask server for 10 TEST messages\n"
        "  /logs all                 (/l a)   full server log buffer\n"
        "  /logs warnings            (/l w)   only WARNING entries\n"
        "  /logs last <minutes>      (/l l N) server logs for last N minutes\n"
        "  /uart <cmd>               (/u)     send command to Arduino via UART\n"
        "  /sensor_all                        all Arduino sensor data\n"
        "  /sensor_last <minutes>             sensor data for last N minutes\n"
        "  /help                     (/h, /?) this help\n"
        "  /exit                     (/q)     quit\n"
        "  Anything else is sent as a chat message.\n\n";
}

// ============================================================
// MAIN LOOP
// ============================================================
static void clientLoop() {
    std::string input;
    while (g_running) {
        printPrompt();
        if (!std::getline(std::cin, input)) {
            g_running = false;
            break;
        }
        if (input.empty()) continue;

        if (input[0] != '/') {
            try {
                if (!g_connected) { std::cout << "Not connected.\n"; continue; }
                sendChat(input);
            } catch (const std::exception& e) {
                std::cout << "Error: " << e.what() << "\n";
            }
            continue;
        }

        std::string rest;
        std::string cmd = normalizeCommand(input, rest);

        try {
            if (cmd == "/connect") {
                if (g_connected) { std::cout << "Already connected.\n"; continue; }
                std::stringstream ss(rest);
                std::string ip; int port = 0;
                ss >> ip >> port;
                if (ip.empty() || port <= 0) {
                    std::cout << "Usage: /connect <ip> <port>\n"; continue;
                }
                connectToServer(ip, port);
            }
            else if (cmd == "/disconnect") {
                disconnectFromServer();
            }
            else if (cmd == "/exit") {
                g_running = false;
                disconnectFromServer();
            }
            else if (cmd == "/help") {
                printHelp();
            }
            else if (cmd == "/temp") {
                if (!g_connected) { std::cout << "Not connected.\n"; continue; }
                sendLogRequest("TEMP");
            }
            else if (cmd == "/status") {
                if (!g_connected) { std::cout << "Not connected.\n"; continue; }
                sendLogRequest("STATUS");
            }
            else if (cmd == "/test") {
                if (!g_connected) { std::cout << "Not connected.\n"; continue; }
                sendLogRequest("TEST");
            }
            else if (cmd == "/logs") {
                if (!g_connected) { std::cout << "Not connected.\n"; continue; }
                std::string payload = buildLogsPayload(rest);
                if (payload.empty()) {
                    std::cout << "Usage: /logs all | /logs warnings | /logs last <minutes>\n";
                    continue;
                }
                sendLogRequest(payload);
            }
            else if (cmd == "/uart") {
                if (!g_connected) { std::cout << "Not connected.\n"; continue; }
                if (rest.empty()) { std::cout << "Usage: /uart <command>\n"; continue; }
                sendUartCommand(rest + "\n");
            }
            else if (cmd == "/sensor_all") {
                if (!g_connected) { std::cout << "Not connected.\n"; continue; }
                sendLogRequest("SENSOR_ALL");
            }
            else if (cmd == "/sensor_last") {
                if (!g_connected) { std::cout << "Not connected.\n"; continue; }
                if (rest.empty()) { std::cout << "Usage: /sensor_last <minutes>\n"; continue; }
                sendLogRequest("SENSOR_LAST " + rest);
            }
            else {
                std::cout << "Unknown command '" << cmd << "'. Type /help.\n";
            }
        }
        catch (const std::exception& e) {
            std::cout << "Error: " << e.what() << "\n";
            g_connected = false;
            if (g_socket != INVALID_SOCKET) {
                closesocket(g_socket);
                g_socket = INVALID_SOCKET;
            }
        }
    }
}

// ============================================================
// MAIN
// ============================================================
int main() {
    setupConsole();
    initWinSock();
    std::cout << "=== Hardware Monitor Client ===\n";
    g_username = askUsername();
    printHelp();
    clientLoop();
    cleanupWinSock();
    return 0;
}