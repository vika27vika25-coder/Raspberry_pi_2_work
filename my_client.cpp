// Client

// Отключает предупреждения об устаревших функциях WinSock
#define _WINSOCK_DEPRECATED_NO_WARNINGS
// Отключает предупреждения CRT об «небезопасных» функциях (localtime, strcpy и др.)
#define _CRT_SECURE_NO_WARNINGS

#include <WinSock2.h>   // Основной заголовок WinSock2: SOCKET, send, recv, connect...
#include <Ws2tcpip.h>   // Расширения TCP/IP: inet_pton, sockaddr_in и др.
#include <windows.h>    // WinAPI: GetStdHandle, SetConsoleMode, HANDLE, DWORD...
#include <iostream>     // std::cout, std::cin, std::endl
#include <string>       // std::string
#include <vector>       // std::vector
#include <thread>       // std::thread (фоновый поток приёма)
#include <mutex>        // std::mutex, std::lock_guard (защита вывода)
#include <atomic>       // std::atomic<bool> (потокобезопасные флаги)
#include <sstream>      // std::stringstream (парсинг строк)
#include <stdexcept>    // std::runtime_error
#include <cstdint>      // uint32_t, uint8_t и др. типы фиксированного размера
#include <ctime>        // time(), localtime_s(), strftime()

// Говорит компоновщику автоматически подключить библиотеку WinSock2
#pragma comment(lib, "Ws2_32.lib")


// ПРОТОКОЛ (совпадает с сервером)
// Перечисление типов сообщений; хранится как uint32_t для совместимости с заголовком
enum class MessageType : uint32_t {
    Text       = 1,  // Обычное чат-сообщение
    Connect    = 2,  // Уведомление о подключении
    Disconnect = 3,  // Уведомление об отключении
    LogRequest = 4,  // Запрос от клиента: TEMP / STATUS / TEST / ALL / WARNINGS / LAST N
    LogResponse= 5,  // Ответ сервера на LogRequest
    Warning    = 6,  // Предупреждение от сервера
    UartSend   = 7,  // клиент шлёт команду на STM
    UartData   = 8   // RPi пересылает ответ STM клиенту
};

// Опасно если клиент на Windows, сервер на Linux (Raspberry Pi)
// Отключаем выравнивание структуры, чтобы она занимала ровно 8 байт в сети
#pragma pack(push, 1)
struct MessageHeader {
    uint32_t type;  // Тип сообщения (одно из значений MessageType)
    uint32_t size;  // Размер тела сообщения в байтах, следующего за заголовком
};
#pragma pack(pop)  // Восстанавливаем стандартное выравнивание

// Максимально допустимый размер тела одного сообщения: 1 МБ (1 << 20 = 1 048 576)
constexpr uint32_t MAX_PAYLOAD = 1u << 20;


// СОСТОЯНИЕ
// Дескриптор активного TCP-сокета; INVALID_SOCKET = «нет соединения»
static SOCKET g_socket = INVALID_SOCKET;
// true, пока соединение с сервером активно
static std::atomic<bool> g_connected{false};
// true, пока главный цикл должен работать; false = выход из программы
static std::atomic<bool> g_running  {true};
// Мьютекс для синхронизации вывода в std::cout между главным и фоновым потоками
static std::mutex g_coutMutex;
// Имя пользователя, введённое при старте; используется в заголовках чат-сообщений
static std::string g_username;


// КОНСОЛЬ И UTF-8
static void setupConsole() {
    SetConsoleOutputCP(CP_UTF8);            // Устанавливаем кодовую страницу вывода на UTF-8
    SetConsoleCP(CP_UTF8);                  // Устанавливаем кодовую страницу ввода на UTF-8
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE); // Получаем хэндл стандартного вывода
    if (hOut != INVALID_HANDLE_VALUE) {     // Проверяем, что хэндл рабочий
        DWORD mode = 0;                     // Переменная для текущего режима консоли
        if (GetConsoleMode(hOut, &mode))    // Читаем текущие флаги режима
            // Добавляем флаг поддержки, используются для цвета предупреждений
            SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}


// ИНИЦИАЛИЗАЦИЯ WinSock2
static void initWinSock() {
    WSADATA wsa;                        // Структура с информацией о версии WinSock
    WSAStartup(MAKEWORD(2, 2), &wsa);   // Инициализируем WinSock версии 2.2
}
// Освобождает ресурсы WinSock при завершении программы
static void cleanupWinSock() { WSACleanup(); }


// УТИЛИТЫ
static std::string getCurrentTime() {
    time_t now = time(nullptr);          // Получаем текущее время в секундах
    struct tm ti;                        // Структура для хранения разобранного времени
    localtime_s(&ti, &now);              // Конвертируем timestamp в локальное время (потокобезопасно)
    char buf[16];                        // Буфер для строки времени
    strftime(buf, sizeof(buf), "%H:%M:%S", &ti); // Форматируем: часы:минуты:секунды
    return buf;                          // Возвращает строку в виде "14:35:07"
}

// Гарантированно отправляет ровно size байт; throw при ошибке сети
static void sendAll(const char* data, int size) {
    int sent = 0;                                       // Счётчик уже отправленных байт
    while (sent < size) {                               // Повторяем, пока не отправлено всё
        int r = send(g_socket, data + sent, size - sent, 0); // Отправляем очередную порцию
        if (r <= 0) throw std::runtime_error("send failed"); // 0 или <0 = обрыв/ошибка
        sent += r;                                      // Учитываем отправленные байты
    }
}

// Гарантированно принимает ровно size байт; throw при ошибке/закрытии соединения
static void recvAll(char* data, int size) {
    int got = 0;                                        // Счётчик уже принятых байт
    while (got < size) {                                // Повторяем, пока не принято всё
        int r = recv(g_socket, data + got, size - got, 0); // Читаем очередную порцию
        if (r <= 0) throw std::runtime_error("recv failed"); // 0 = сервер закрыл соединение
        got += r;                                       // Учитываем принятые байты
    }
}


// ОТПРАВКА
static void sendChat(const std::string& input) {
    // Формируем тело: username HH:MM:SS
    std::string body = "[" + g_username + " " + getCurrentTime() + "] " + input;
    MessageHeader header{ (uint32_t)MessageType::Text, (uint32_t)body.size() }; // Заголовок типа Text
    sendAll((char*)&header, sizeof(header));                // Отправляем заголовок
    sendAll(body.data(), (int)body.size());                 // Отправляем тело сообщения
}

static void sendLogRequest(const std::string& payload) {
    // Формируем заголовок запроса; payload = "TEMP", "STATUS", "ALL", "WARNINGS", "LAST N"
    MessageHeader header{ (uint32_t)MessageType::LogRequest,
                          (uint32_t)payload.size() };
    sendAll((char*)&header, sizeof(header));                // Отправляем заголовок
    if (!payload.empty()) sendAll(payload.data(), (int)payload.size()); // Отправляем тело (если есть)
}

// ИСПРАВЛЕНО: перенесено после sendAll и MessageHeader, которые она использует
static void sendUartCommand(const std::string& cmd) {
    MessageHeader header{ (uint32_t)MessageType::UartSend, (uint32_t)cmd.size() };
    sendAll((char*)&header, sizeof(header));
    sendAll(cmd.data(), (int)cmd.size());
}


// ВВОД ИМЕНИ
static std::string askUsername() {
    while (true) {                                          // Бесконечный цикл до ввода корректного имени
        std::cout << "Enter your name (3-20 characters): "; // Приглашение к вводу
        std::string name;                                   // Переменная для хранения введённого имени
        std::getline(std::cin, name);                       // Читаем всю строку (включая пробелы)
        size_t first = name.find_first_not_of(" \t");       // Ищем первый непробельный символ
        if (first == std::string::npos) {                   // Если строка пустая или только пробелы
            std::cout << "Name cannot be empty.\n";         // Сообщаем об ошибке
            continue;                                       // Просим ввести снова
        }
        size_t last = name.find_last_not_of(" \t");         // Ищем последний непробельный символ
        name = name.substr(first, last - first + 1);        // Обрезаем пробелы с обоих концов (trim)
        if (name.length() < 3 || name.length() > 20) {     // Проверяем длину после обрезки
            std::cout << "Length must be between 3 and 20.\n"; // Сообщаем об ошибке длины
            continue;                                       // Просим ввести снова
        }
        return name;                                        // Имя корректно — возвращаем его
    }
}


// ПОТОК ПРИЁМА
// Выводит приглашение "> " без перевода строки (flush нужен для немедленного отображения)
static void printPrompt() { std::cout << "> " << std::flush; }

// Фоновый поток: непрерывно читает сообщения от сервера и выводит их в консоль
static void receiveLoop() {
    try {
        while (g_connected) {                               // Крутимся, пока соединение активно
            MessageHeader header{};                         // Буфер для заголовка входящего сообщения
            recvAll((char*)&header, sizeof(header));        // Читаем ровно 8 байт заголовка
            if (header.size > MAX_PAYLOAD)                  // Защита: отвергаем слишком большие пакеты
                throw std::runtime_error("payload too big");

            std::vector<char> data(header.size);            // Выделяем буфер под тело сообщения
            if (header.size > 0) recvAll(data.data(), (int)header.size); // Читаем тело (если оно есть)
            std::string text(data.begin(), data.end());     // Конвертируем байты в строку

            std::lock_guard<std::mutex> lock(g_coutMutex); // Захватываем мьютекс вывода
            switch ((MessageType)header.type) {             // Выбираем формат вывода по типу сообщения
                case MessageType::LogResponse:              // Ответ на запрос логов/статуса
                    std::cout << "\n--- response ---\n"
                              << text
                              << "----------------\n";
                    break;
                case MessageType::Warning:                  // Предупреждение от сервера
                    std::cout << "\n\033[31m[!! SERVER WARNING] " // красный цвет
                              << text << "\033[0m\n";       // сброс цвета
                    break;
                case MessageType::UartData:                 // RPi пересылает ответ STM клиенту
                    std::cout << "\n[STM] " << text << "\n";
                    break;
                case MessageType::Text:                     // Обычное чат-сообщение
                    std::cout << "\n" << text << "\n";
                    break;
                default:                                    // Неизвестный тип — тоже как текст
                    std::cout << "\n" << text << "\n";
                    break;
            }
            printPrompt();                                  // Восстанавливаем приглашение "> " после вывода
        }
    }
    catch (...) {                                           // Перехватываем любое исключение (обрыв сети)
        std::lock_guard<std::mutex> lock(g_coutMutex);     // Захватываем мьютекс для вывода ошибки
        std::cout << "\n[disconnected from server]\n";      // Сообщаем пользователю об обрыве
        printPrompt();                                      // Восстанавливаем приглашение
        g_connected = false;                                // Сбрасываем флаг соединения
        closesocket(g_socket);                              // Закрываем сокет
        g_socket = INVALID_SOCKET;                          // Помечаем сокет как недействительный
    }
}


// CONNECT / DISCONNECT
static bool connectToServer(const std::string& ip, int port) {
    g_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);   // Создаём TCP-сокет (IPv4)
    if (g_socket == INVALID_SOCKET) {                       // Если создать не удалось
        std::cout << "socket() failed\n";                   // Выводим ошибку
        return false;                                       // Возвращаем неудачу
    }
    sockaddr_in addr{};                                     // Структура адреса сервера (обнуляем)
    addr.sin_family = AF_INET;                              // Протокол: IPv4
    addr.sin_port   = htons((u_short)port);                 // Порт в сетевом порядке байт (big-endian)
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) { // Конвертируем строку IP в бинарный вид
        std::cout << "Invalid IP address.\n";               // IP-адрес имеет неверный формат
        closesocket(g_socket);                              // Закрываем только что созданный сокет
        g_socket = INVALID_SOCKET;                          // Помечаем как недействительный
        return false;
    }
    if (connect(g_socket, (sockaddr*)&addr, sizeof(addr)) != 0) { // Устанавливаем TCP-соединение
        std::cout << "Connection failed (code " << WSAGetLastError() << ").\n"; // Ошибка подключения
        closesocket(g_socket);                              // Закрываем сокет
        g_socket = INVALID_SOCKET;                          // Помечаем как недействительный
        return false;
    }
    try {
        // Формируем заголовок приветственного сообщения (тип Connect, размер = длина имени)
        MessageHeader header{ (uint32_t)MessageType::Connect,
                              (uint32_t)g_username.size() };
        sendAll((char*)&header, sizeof(header));            // Отправляем 8-байтный заголовок
        sendAll(g_username.data(), (int)g_username.size()); // Отправляем имя пользователя как тело
    }
    catch (...) {                                           // Если отправка не удалась
        std::cout << "Failed to send hello.\n";             // Сообщаем об ошибке
        closesocket(g_socket);                              // Закрываем сокет
        g_socket = INVALID_SOCKET;                          // Помечаем как недействительный
        return false;
    }
    g_connected = true;                                     // Соединение установлено — поднимаем флаг
    std::thread(receiveLoop).detach();                      // Запускаем фоновый поток чтения (detach = не ждём завершения)
    std::cout << "Connected to " << ip << ":" << port
              << " as '" << g_username << "'\n";            // Информируем пользователя об успехе
    return true;                                            // Возвращаем успех
}

static void disconnectFromServer() {
    if (!g_connected) return;                               // Если не подключены — ничего не делаем
    try {
        // Формируем заголовок сообщения Disconnect (тело = имя пользователя)
        MessageHeader header{ (uint32_t)MessageType::Disconnect,
                              (uint32_t)g_username.size() };
        sendAll((char*)&header, sizeof(header));            // Отправляем заголовок серверу
        sendAll(g_username.data(), (int)g_username.size()); // Отправляем имя (сервер узнает, кто ушёл)
    } catch (...) {}                                        // Игнорируем ошибки: сеть могла уже упасть
    g_connected = false;                                    // Сбрасываем флаг соединения
    if (g_socket != INVALID_SOCKET) {                       // Если сокет ещё открыт
        closesocket(g_socket);                              // Закрываем его
        g_socket = INVALID_SOCKET;                          // Помечаем как недействительный
    }
    std::cout << "Disconnected.\n";                         // Подтверждаем пользователю отключение
}


// НОРМАЛИЗАЦИЯ КОМАНД
// Возвращает каноническую форму команды (первое слово), всё остальное
// в `rest`. Если ввод не похож на команду — возвращает "".
static std::string normalizeCommand(const std::string& input, std::string& rest) {
    if (input.empty() || input[0] != '/') return "";        // Не команда (нет '/') → вернуть ""

    // Разделим на первое слово и хвост.
    size_t sp = input.find(' ');                            // Ищем первый пробел
    std::string head = (sp == std::string::npos) ? input : input.substr(0, sp); // Слово до пробела
    rest             = (sp == std::string::npos) ? "" : input.substr(sp + 1);   // Всё после пробела

    // Приведём первое слово к нижнему регистру для устойчивости.
    for (auto& ch : head) ch = (char)tolower((unsigned char)ch); // Нижний регистр для case-insensitive сравнения

    // Алиасы → канон.
    // Принцип: канонические команды + однобуквенные/двухбуквенные сокращения.
    // Где возможна двусмысленность, разруливаем явно (например '/s' = /status,
    // потому что у нас одна команда на 's', /disconnect использует /d).
    static const std::vector<std::pair<std::string, std::string>> table = {
        {"/connect",    "/connect"},   {"/c",  "/connect"},   // /c → /connect
        {"/disconnect", "/disconnect"},{"/d",  "/disconnect"}, // /d → /disconnect
        {"/exit",       "/exit"},      {"/q",  "/exit"},     {"/quit", "/exit"}, // /q, /quit → /exit
        {"/help",       "/help"},      {"/h",  "/help"},     {"/?",    "/help"}, // /h, /? → /help
        {"/temp",       "/temp"},      {"/t",  "/temp"},     // /t → /temp
        {"/status",     "/status"},    {"/s",  "/status"},   // /s → /status
        {"/test",       "/test"},                             // алиасов нет
        {"/logs",       "/logs"},      {"/l",  "/logs"},     // /l → /logs
        {"/uart",       "/uart"},      {"/u",  "/uart"},     // /u → /uart
    };
    for (auto& p : table)                                   // Перебираем всю таблицу алиасов
        if (head == p.first) return p.second;               // Нашли совпадение → возвращаем канон

    return head; // для сообщения об ошибке
}

// Внутри /logs тоже принимаем сокращения подкоманд.
// Возвращает payload для сервера ("ALL", "WARNINGS", "LAST N"), или "" если ошибка.
static std::string buildLogsPayload(const std::string& rest) {
    if (rest.empty()) return "";                            // Нет аргументов — неверный вызов
    std::stringstream ss(rest);                             // Парсим аргументы через поток
    std::string sub;                                        // Переменная для подкоманды
    ss >> sub;                                              // Читаем первое слово (подкоманду)
    for (auto& ch : sub) ch = (char)tolower((unsigned char)ch); // Нижний регистр для сравнения

    if (sub == "all" || sub == "a")           return "ALL";         // /logs all или /l a
    if (sub == "warnings" || sub == "warn"
        || sub == "w")                        return "WARNINGS";    // /logs warnings / warn / w
    if (sub == "last" || sub == "l") {                              // /logs last N или /l l N
        int minutes = 0;                                            // Количество минут
        if (!(ss >> minutes) || minutes <= 0) return "";            // Нет числа или число ≤ 0 → ошибка
        return "LAST " + std::to_string(minutes);                   // Формируем "LAST <N>"
    }
    return "";                                              // Неизвестная подкоманда → ошибка
}


// HELP
static void printHelp() {
    std::cout <<                                            // Выводим справку одной строковой константой
        "\nCommands (short aliases in parentheses):\n"
        "  /connect <ip> <port>     (/c)    connect to the Raspberry server\n"
        "  /disconnect              (/d)    close current connection\n"
        "  /temp                    (/t)    current CPU temperature\n"
        "  /status                  (/s)    full status (CPU/RAM/uptime/clients)\n"
        "  /test                            ask server for 10 TEST messages\n"
        "  /logs all                (/l a)  full log buffer\n"
        "  /logs warnings           (/l w)  only WARNING entries\n"
        "  /logs last <minutes>     (/l l N) logs for last N minutes\n"
        "  /help                    (/h, /?)this help\n"
        "  /exit                    (/q)    quit\n"
        "  /uart <cmd>              (/u)    send command to STM/Arduino via UART\n"
        "  Anything else is sent as a chat message.\n\n";
}


// MAIN LOOP
static void clientLoop() {
    std::string input;                                      // Буфер для строки ввода пользователя
    while (g_running) {                                     // Работаем, пока не /exit или EOF
        printPrompt();                                      // Выводим приглашение "> "
        if (!std::getline(std::cin, input)) {               // Читаем строку; false = EOF (Ctrl+Z / Ctrl+D)
            g_running = false;                              // Помечаем завершение
            break;                                          // Выходим из цикла
        }
        if (input.empty()) continue;                        // Пустая строка — просто снова показываем "> "

        // Если строка не начинается с '/', это чат-сообщение.
        if (input[0] != '/') {
            try {
                if (!g_connected) { std::cout << "Not connected.\n"; continue; } // Нельзя отправить без соединения
                sendChat(input);                            // Отправляем сообщение как чат
            } catch (const std::exception& e) {
                std::cout << "Error: " << e.what() << "\n"; // Ошибка сети при отправке
            }
            continue;                                       // Переходим к следующей итерации
        }

        std::string rest;                                   // Аргументы после команды
        std::string cmd = normalizeCommand(input, rest);    // Получаем каноническую команду и аргументы

        try {
            if (cmd == "/connect") {                        // Команда подключения к серверу
                if (g_connected) { std::cout << "Already connected.\n"; continue; } // Уже подключены
                std::stringstream ss(rest);                 // Парсим аргументы: ip port
                std::string ip;                             // IP-адрес сервера
                int port = 0;                               // Порт сервера
                ss >> ip >> port;                           // Извлекаем IP и порт из строки аргументов
                if (ip.empty() || port <= 0) {              // Если аргументы не указаны или неверны
                    std::cout << "Usage: /connect <ip> <port>   (or /c <ip> <port>)\n";
                    continue;
                }
                connectToServer(ip, port);                  // Выполняем подключение
            }
            else if (cmd == "/disconnect") {                // Команда отключения от сервера
                disconnectFromServer();
            }
            else if (cmd == "/exit") {                      // Команда выхода из программы
                g_running = false;                          // Сигнализируем главному циклу о завершении
                disconnectFromServer();                     // Корректно закрываем соединение
            }
            else if (cmd == "/help") {                      // Команда вывода справки
                printHelp();
            }
            else if (cmd == "/temp") {                      // Запрос текущей температуры CPU
                if (!g_connected) { std::cout << "Not connected.\n"; continue; }
                sendLogRequest("TEMP");                     // Шлём серверу запрос "TEMP"
            }
            else if (cmd == "/status") {                    // Запрос полного статуса системы
                if (!g_connected) { std::cout << "Not connected.\n"; continue; }
                sendLogRequest("STATUS");                   // Шлём серверу запрос "STATUS"
            }
            else if (cmd == "/test") {                      // Запрос тестовых сообщений от сервера
                if (!g_connected) { std::cout << "Not connected.\n"; continue; }
                sendLogRequest("TEST");                     // Шлём серверу запрос "TEST"
            }
            else if (cmd == "/logs") {                      // Запрос логов с фильтром
                if (!g_connected) { std::cout << "Not connected.\n"; continue; }
                std::string payload = buildLogsPayload(rest); // Строим payload из подкоманды и аргументов
                if (payload.empty()) {                      // Если подкоманда неверна — показываем подсказку
                    std::cout << "Usage: /logs all | /logs warnings | "
                                 "/logs last <minutes>\n"
                                 "       (short: /l a | /l w | /l l <minutes>)\n";
                    continue;
                }
                sendLogRequest(payload);                    // Отправляем запрос на сервер
            }
            else if (cmd == "/uart") {                      // Команда отправки данных на STM через UART
                if (!g_connected) { std::cout << "Not connected.\n"; continue; }
                if (rest.empty()) { std::cout << "Usage: /uart <command>\n"; continue; }
                sendUartCommand(rest + "\n");               // \n как терминатор для STM
            }
            else {
                std::cout << "Unknown command '" << cmd << "'. Type /help.\n"; // Неизвестная команда
            }
        }
        catch (const std::exception& e) {                  // Обрабатываем сетевые и прочие ошибки
            std::cout << "Error: " << e.what() << "\n";    // Выводим описание ошибки
            g_connected = false;                            // Считаем соединение разорванным
            if (g_socket != INVALID_SOCKET) {               // Если сокет ещё открыт
                closesocket(g_socket);                      // Закрываем его
                g_socket = INVALID_SOCKET;                  // Помечаем как недействительный
            }
        }
    }
}


int main() {
    setupConsole();                                         // Настройка UTF-8 и ANSI в консоли
    initWinSock();                                          // Инициализация WinSock2
    std::cout << "=== Hardware Monitor Client ===\n";       // Приветственный баннер
    g_username = askUsername();                             // Запрашиваем и сохраняем имя пользователя
    printHelp();                                            // Выводим справку по командам
    clientLoop();                                           // Запускаем основной цикл обработки ввода
    cleanupWinSock();                                       // Освобождаем ресурсы WinSock при выходе
    return 0;                                               // Код возврата 0 = успешное завершение
}