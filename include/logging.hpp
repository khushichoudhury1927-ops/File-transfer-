#ifndef LOGGING_HPP
#define LOGGING_HPP

#include <cstddef>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

// The server drives its io_context from a pool of threads, so several sessions
// can log at the same instant. Every line is therefore formatted into a buffer
// first and written under a single lock, which keeps concurrent sessions from
// interleaving halves of each other's lines.
//
// A progress line is the awkward case: it is rewritten in place and carries no
// newline, so anything else written while it is on screen would land on the
// same row. It is therefore erased first, and every progress update is padded
// to the widest text it has already used, so the field never shrinks.
namespace logging {

// In-place progress only makes sense on a terminal. With the output redirected
// to a file every update would be kept, so a single transfer would bury the log
// in hundreds of lines that all say the same thing. When it is not a terminal
// the bar is dropped and only the real log lines are written.
inline bool stdout_is_terminal() {
    static const bool value = [] {
#ifdef _WIN32
        const HANDLE handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
            return false;
        }
        DWORD mode = 0;
        return ::GetConsoleMode(handle, &mode) != 0;
#else
        return ::isatty(STDOUT_FILENO) != 0;
#endif
    }();
    return value;
}

inline std::mutex& mutex() {
    static std::mutex instance;
    return instance;
}

// Width of the progress line currently on screen; 0 when there is none.
inline std::size_t& progress_width() {
    static std::size_t width = 0;
    return width;
}

// Blanks the current progress line. The caller must hold mutex().
inline void erase_progress_locked() {
    std::size_t& width = progress_width();
    if (width != 0) {
        std::cout << '\r' << std::string(width, ' ') << '\r' << std::flush;
        width = 0;
    }
}

template <typename Builder>
void info(Builder&& build) {
    std::ostringstream out;
    build(out);
    const std::string text = out.str();
    const std::lock_guard<std::mutex> lock(mutex());
    erase_progress_locked();
    std::cout << text << '\n';
}

template <typename Builder>
void error(Builder&& build) {
    std::ostringstream out;
    build(out);
    const std::string text = out.str();
    const std::lock_guard<std::mutex> lock(mutex());
    erase_progress_locked();
    std::cerr << text << '\n';
}

// A transfer's progress line is rewritten in place, so it carries no newline.
inline void progress(const std::string& text) {
    if (!stdout_is_terminal()) {
        return;
    }
    const std::lock_guard<std::mutex> lock(mutex());
    std::size_t& width = progress_width();
    if (text.size() > width) {
        width = text.size();
    }
    std::cout << '\r' << text;
    if (text.size() < width) {
        std::cout << std::string(width - text.size(), ' ');
    }
    std::cout << std::flush;
}

// Clears the progress line before anything else is written.
inline void clear_progress() {
    const std::lock_guard<std::mutex> lock(mutex());
    erase_progress_locked();
}

}  // namespace logging

#endif // LOGGING_HPP
