#pragma once
#include <string>

namespace motion::tui {

enum class Key {
    Up, Down, Left, Right,
    Enter, Escape, Backspace, Tab,
    Char, Unknown
};

struct KeyEvent {
    Key  key = Key::Unknown;
    char ch  = 0;
};

namespace color {
    constexpr const char* reset      = "\x1b[0m";
    constexpr const char* bold       = "\x1b[1m";
    constexpr const char* dim        = "\x1b[2m";
    constexpr const char* red        = "\x1b[31m";
    constexpr const char* green      = "\x1b[32m";
    constexpr const char* yellow     = "\x1b[33m";
    constexpr const char* blue       = "\x1b[34m";
    constexpr const char* magenta    = "\x1b[35m";
    constexpr const char* cyan       = "\x1b[36m";
    constexpr const char* white      = "\x1b[37m";
    constexpr const char* brightCyan = "\x1b[96m";
    constexpr const char* gray       = "\x1b[90m";
    constexpr const char* invert     = "\x1b[7m";
}

class Frame {
public:
    Frame& line(const std::string& s = "");
    Frame& raw(const std::string& s);
    Frame& write(char c);

    void clear() { m_buf.clear(); }
    const std::string& str() const { return m_buf; }

private:
    std::string m_buf;
};

class Terminal {
public:
    Terminal();
    ~Terminal();

    void present(const Frame& frame) const;
    int  rows() const;

    void clearScreen() const;
    void hideCursor() const;
    void showCursor() const;
    void moveTo(int row, int col) const;
    void setTitle(const std::wstring& title) const;

    KeyEvent readKey() const;
    std::string readLineAt(int row, int col, const std::string& initial = "") const;

    void write(const std::string& s) const;

private:
    void* m_outHandle = nullptr;
    void* m_inHandle  = nullptr;
    unsigned long m_savedOutMode = 0;
    unsigned long m_savedInMode  = 0;
    unsigned int  m_savedCp      = 0;
};

}
