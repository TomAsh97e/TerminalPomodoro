#include <charconv>
#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace {
constexpr std::string_view reset = "\033[0m";
constexpr std::string_view bold = "\033[1m";
constexpr std::string_view pink = "\033[38;2;255;183;197m";
constexpr std::string_view peach = "\033[38;2;255;218;185m";
constexpr std::string_view mint = "\033[38;2;183;240;211m";
constexpr std::string_view lavender = "\033[38;2;210;190;255m";
constexpr std::string_view muted = "\033[38;2;112;108;130m";
constexpr std::string_view red = "\033[38;2;255;160;170m";

void usage(std::ostream& out) {
    out << "Użycie: PomodoroT --nauka MINUTY --przerwa MINUTY\n";
}

bool parseMinutes(std::string_view text, int& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() && value >= 1 && value <= 1440;
}

bool parseArguments(int argc, char* argv[], int& studyMinutes, int& breakMinutes) {
    if (argc != 5) return false;

    bool hasStudy = false;
    bool hasBreak = false;
    for (int i = 1; i < argc; i += 2) {
        const std::string_view option = argv[i];
        int value = 0;
        if (!parseMinutes(argv[i + 1], value)) return false;

        if (option == "--nauka" && !hasStudy) {
            studyMinutes = value;
            hasStudy = true;
        } else if (option == "--przerwa" && !hasBreak) {
            breakMinutes = value;
            hasBreak = true;
        } else {
            return false;
        }
    }
    return hasStudy && hasBreak;
}

std::string formatTime(int seconds) {
    const int hours = seconds / 3600;
    const int minutes = seconds / 60 % 60;
    const int remainingSeconds = seconds % 60;
    std::string result;
    if (hours > 0) result = std::to_string(hours) + ':';
    if (hours > 0 && minutes < 10) result += '0';
    result += std::to_string(minutes) + ':';
    if (remainingSeconds < 10) result += '0';
    return result + std::to_string(remainingSeconds);
}

class RawTerminal {
public:
    RawTerminal() {
        if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &original_) != 0) return;
        termios raw = original_;
        raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG));
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        enabled_ = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
        if (enabled_) std::cout << "\033[?25l" << std::flush;
    }

    ~RawTerminal() {
        if (enabled_) {
            tcsetattr(STDIN_FILENO, TCSANOW, &original_);
            std::cout << "\033[?25h" << std::flush;
        }
    }

    int readKey(int timeoutMilliseconds) const {
        if (!enabled_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMilliseconds));
            return 0;
        }
        pollfd input{STDIN_FILENO, POLLIN, 0};
        if (poll(&input, 1, timeoutMilliseconds) <= 0) return 0;
        char key = 0;
        return read(STDIN_FILENO, &key, 1) == 1 ? key : 0;
    }

private:
    termios original_{};
    bool enabled_ = false;
};

int terminalColumns() {
    winsize size{};
    return ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 2 ? size.ws_col : 80;
}

void breathingDot(std::string_view color, bool paused, bool finished, int frame) {
    int red = 210;
    int green = 190;
    int blue = 255;
    if (paused) {
        red = 255;
        green = 218;
        blue = 185;
    } else if (color == pink) {
        red = 255;
        green = 183;
        blue = 197;
    } else if (color == mint) {
        red = 183;
        green = 240;
        blue = 211;
    }

    const int phase = frame % 50;
    const int strength = finished ? 25 : (phase <= 25 ? phase : 50 - phase);
    const auto brightness = [strength](int channel) {
        return channel * (55 * 25 + 45 * strength) / 2500;
    };
    std::cout << "\033[38;2;" << brightness(red) << ';' << brightness(green) << ';'
              << brightness(blue) << "m●";
}

void renderTimer(std::string_view label, std::string_view color, int remainingSeconds,
                 long long elapsedMilliseconds, long long totalMilliseconds,
                 bool paused, bool finished, int frame, bool redraw) {
    const int columns = terminalColumns();
    const int width = columns - 2;
    const int filled = static_cast<int>(elapsedMilliseconds * width / totalMilliseconds);

    if (redraw) std::cout << "\033[4A";
    std::cout << '\r' << pink << bold << "POMODOROT" << reset << muted << "  ·  "
              << color << label << reset << "\033[K\n";
    std::cout << '\r' << color << bold << formatTime(remainingSeconds) << reset << "\033[K\n";
    std::cout << "\r " << color;
    for (int i = 0; i < filled; ++i) std::cout << "█";
    std::cout << muted;
    for (int i = filled; i < width; ++i) std::cout << "░";
    std::cout << reset << "\033[K\n\r ";
    breathingDot(color, paused, finished, frame);
    std::cout << "  " << bold
              << (finished ? "GOTOWE" : paused ? "PAUZA · p: wznowienie" : "AKTYWNA · p: pauza")
              << reset << "\033[K\n" << std::flush;
}

bool runTimer(int minutes, std::string_view label, std::string_view color) {
    using Clock = std::chrono::steady_clock;
    using Milliseconds = std::chrono::milliseconds;
    const auto total = std::chrono::minutes(minutes);
    const long long totalMilliseconds = std::chrono::duration_cast<Milliseconds>(total).count();
    auto runningSince = Clock::now();
    Milliseconds activeTime{0};
    bool paused = false;
    bool redraw = false;
    int frame = 0;
    RawTerminal terminal;

    while (true) {
        const auto now = Clock::now();
        const auto elapsed = activeTime + (paused ? Milliseconds{0}
                                                  : std::chrono::duration_cast<Milliseconds>(now - runningSince));
        const bool finished = elapsed >= total;
        const auto shownElapsed = finished ? std::chrono::duration_cast<Milliseconds>(total) : elapsed;
        const int remaining = finished ? 0 : minutes * 60 - static_cast<int>(shownElapsed.count() / 1000);
        renderTimer(label, color, remaining, shownElapsed.count(), totalMilliseconds,
                    paused, finished, frame++, redraw);
        redraw = true;
        if (finished) return true;

        const int key = terminal.readKey(80);
        if (key == 3) return false; // Ctrl+C
        if (key != 'p' && key != 'P') continue;

        if (paused) {
            runningSince = Clock::now();
            paused = false;
        } else {
            activeTime += std::chrono::duration_cast<Milliseconds>(Clock::now() - runningSince);
            paused = true;
        }
    }
}

bool waitForEnter(std::string_view message) {
    std::cout << peach << message << reset << std::flush;
    std::string line;
    return static_cast<bool>(std::getline(std::cin, line));
}

bool collectTasks(std::vector<std::string>& tasks) {
    if (!tasks.empty()) {
        std::cout << '\n' << lavender << bold << "Zadania przeniesione z poprzedniej sesji:" << reset << '\n';
        for (const auto& task : tasks) std::cout << mint << "  • " << reset << task << '\n';
    }

    std::cout << '\n' << lavender << bold << "Wpisz zadania na tę sesję" << reset
              << muted << " (po jednym wierszu, pusty wiersz kończy):" << reset << '\n';
    while (true) {
        std::cout << pink << "› " << reset << std::flush;
        std::string task;
        if (!std::getline(std::cin, task)) return false;
        if (task.empty()) {
            if (!tasks.empty()) return true;
            std::cout << red << "Dodaj przynajmniej jedno zadanie.\n" << reset;
        } else {
            tasks.push_back(std::move(task));
        }
    }
}

bool reviewTasks(std::vector<std::string>& tasks) {
    std::vector<std::string> unfinished;
    std::cout << '\n' << lavender << bold << "Podsumowanie zadań:" << reset << '\n';

    for (auto& task : tasks) {
        while (true) {
            std::cout << peach << "Czy ukończono „" << reset << task << peach << "”? [t/n]: " << reset << std::flush;
            std::string answer;
            if (!std::getline(std::cin, answer)) return false;
            if (answer == "t" || answer == "T" || answer == "tak" || answer == "TAK") break;
            if (answer == "n" || answer == "N" || answer == "nie" || answer == "NIE" || answer.empty()) {
                unfinished.push_back(std::move(task));
                break;
            }
            std::cout << red << "Wpisz t albo n.\n" << reset;
        }
    }
    tasks = std::move(unfinished);
    return true;
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
        usage(std::cout);
        return 0;
    }

    int studyMinutes = 0;
    int breakMinutes = 0;
    if (!parseArguments(argc, argv, studyMinutes, breakMinutes)) {
        std::cerr << red << "Błędne argumenty. Minuty muszą mieścić się w zakresie 1–1440.\n" << reset;
        usage(std::cerr);
        return 1;
    }

    std::vector<std::string> tasks;
    std::cout << pink << bold << "PomodoroT" << reset << muted
              << " · p: pauza/wznowienie · Ctrl+C: koniec" << reset << '\n';

    while (collectTasks(tasks)) {
        if (!waitForEnter("Naciśnij Enter, aby rozpocząć naukę... ")) break;
        if (!runTimer(studyMinutes, "NAUKA", pink)) break;
        if (!reviewTasks(tasks)) break;
        if (!waitForEnter("\nNaciśnij Enter, aby rozpocząć przerwę... ")) break;
        if (!runTimer(breakMinutes, "PRZERWA", mint)) break;
    }

    std::cout << '\n' << lavender << "Do zobaczenia!" << reset << '\n';
}
