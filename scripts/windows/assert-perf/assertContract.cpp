#include <cstring>
#include <iostream>
#include <stdexcept>
#include <cassert>
#include "dsp56kBase/dspassert.h"

namespace
{
    int reports = 0;
    int messages = 0;
    const char* lastMessage = nullptr;
    const char* nextMessage() { ++messages; return "expected failure"; }
    void require(bool condition)
    {
        if(!condition) throw std::runtime_error("assertion macro contract failed");
    }
}

// A test handler makes Debug failure routing observable without an abort/dialog.
void dsp56k::Assert::show(const char* message, const char* function, int line)
{
    require(function != nullptr && line > 0);
    ++reports;
    lastMessage = message;
}

int main()
{
    int evaluations = 0;
    if(true)
        assert(++evaluations == 1);
    else
        return 10;
    assert(++evaluations == 0);
    assertf(++evaluations == 0, nextMessage());
#ifdef _DEBUG
    require(evaluations == 3 && reports == 2 && messages == 1);
    require(std::strcmp(lastMessage, "expected failure") == 0);
    std::cout << "Debug: arguments evaluated once; failed assertions reach handler\n";
#else
    require(evaluations == 0 && reports == 0 && messages == 0);
    std::cout << "Release: arguments and diagnostic messages are not evaluated\n";
#endif
}
