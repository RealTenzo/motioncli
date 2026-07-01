#include "tui/terminal.h"

#include <windows.h>
#include <conio.h>

#include <cstdio>

namespace motion::tui {

Frame& Frame::line(const std::string& s) {
    m_buf.append(s);
    m_buf.append("\x1b[K\r\n");
    return *this;
}

Frame& Frame::raw(const std::string& s) {
    m_buf.append(s);
    return *this;
}

Frame& Frame::write(char c) {
    m_buf.push_back(c);
    return *this;
}

Terminal::Terminal() {
    m_outHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    m_inHandle  = GetStdHandle(STD_INPUT_HANDLE);

    GetConsoleMode((HANDLE)m_outHandle, &m_savedOutMode);
    GetConsoleMode((HANDLE)m_inHandle, &m_savedInMode);
    m_savedCp = GetConsoleOutputCP();

    DWORD outMode = m_savedOutMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode((HANDLE)m_outHandle, outMode);

    SetConsoleOutputCP(CP_UTF8);

    clearScreen();
}

Terminal::~Terminal() {
    showCursor();
    write(color::reset);
    if (m_outHandle) SetConsoleMode((HANDLE)m_outHandle, m_savedOutMode);
    if (m_inHandle)  SetConsoleMode((HANDLE)m_inHandle, m_savedInMode);
    if (m_savedCp)   SetConsoleOutputCP(m_savedCp);
}

void Terminal::present(const Frame& frame) const {
    static std::string out;
    out.clear();
    out.reserve(frame.str().size() + 24);
    out.append("\x1b[?25l\x1b[H");
    out.append(frame.str());
    out.append("\x1b[0J");
    write(out);
}

int Terminal::rows() const {
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (GetConsoleScreenBufferInfo((HANDLE)m_outHandle, &csbi))
        return csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    return 25;
}

void Terminal::clearScreen() const { write("\x1b[2J\x1b[3J\x1b[H"); }
void Terminal::hideCursor() const  { write("\x1b[?25l"); }
void Terminal::showCursor() const  { write("\x1b[?25h"); }

void Terminal::moveTo(int row, int col) const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row, col);
    write(buf);
}

void Terminal::setTitle(const std::wstring& title) const {
    SetConsoleTitleW(title.c_str());
}

void Terminal::write(const std::string& s) const {
    DWORD written = 0;
    WriteFile((HANDLE)m_outHandle, s.data(), (DWORD)s.size(), &written, nullptr);
}

KeyEvent Terminal::readKey() const {
    KeyEvent ev;
    int c = _getch();

    if (c == 0x00 || c == 0xE0) {
        int c2 = _getch();
        switch (c2) {
            case 72: ev.key = Key::Up;    break;
            case 80: ev.key = Key::Down;  break;
            case 75: ev.key = Key::Left;  break;
            case 77: ev.key = Key::Right; break;
            default: ev.key = Key::Unknown; break;
        }
        return ev;
    }

    switch (c) {
        case '\r': case '\n': ev.key = Key::Enter;     return ev;
        case 27:              ev.key = Key::Escape;    return ev;
        case '\b': case 127:  ev.key = Key::Backspace; return ev;
        case '\t':            ev.key = Key::Tab;       return ev;
        default:
            ev.key = Key::Char;
            ev.ch  = (char)c;
            return ev;
    }
}

std::string Terminal::readLineAt(int row, int col, const std::string& initial) const {
    std::string buffer = initial;
    showCursor();
    moveTo(row, col);
    write(buffer);

    while (true) {
        KeyEvent ev = readKey();
        if (ev.key == Key::Enter) {
            break;
        } else if (ev.key == Key::Escape) {
            buffer.clear();
            break;
        } else if (ev.key == Key::Backspace) {
            if (!buffer.empty()) {
                buffer.pop_back();
                write("\b \b");
            }
        } else if (ev.key == Key::Char && (unsigned char)ev.ch >= 0x20) {
            buffer.push_back(ev.ch);
            char tmp[2] = { ev.ch, 0 };
            write(tmp);
        }
    }

    hideCursor();
    return buffer;
}

}
