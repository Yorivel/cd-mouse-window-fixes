#pragma once

// Plain-text log next to the .asi (MKCursorTooltip.log), truncated each launch.
namespace logx {
void open(const wchar_t* path);
void write(const char* fmt, ...);
void close(); // stops logging (the file can then be deleted)
}

#define LOG(...) ::logx::write(__VA_ARGS__)
