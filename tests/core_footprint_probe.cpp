// A program that links neograph::core and nothing else must not load libcurl.
//
// Core drives provider calls through SchemaProvider's runtime client, but only neograph::llm
// constructs one, so only neograph::llm needs the libcurl backend. When the runtime library
// pulled the transport in, every graph-only process loaded libcurl and the roughly 25 libraries
// it needs (about 8 MB resident at start) for nothing.
//
// The probe takes the address of a core entry point that reaches the runtime client, so the
// linker keeps the provider code (and anything it depends on) in the binary, then looks for
// libcurl among the mapped files. Linux only: it reads /proc/self/maps.
#include <neograph/provider.h>

#include <fstream>
#include <iostream>
#include <string>

int main() {
    // Reference, never call: forces provider.cpp and, through it, sp::runtime::Client::start.
    volatile auto keep = &neograph::Provider::dispatch;
    (void)keep;

    std::ifstream maps("/proc/self/maps");
    if (!maps) {
        std::cerr << "cannot read /proc/self/maps\n";
        return 2;
    }
    std::string line;
    bool mapped_any = false;
    while (std::getline(maps, line)) {
        mapped_any = true;
        if (line.find("libcurl") != std::string::npos) {
            std::cerr << "libcurl is mapped into a core-only program:\n  " << line << '\n';
            return 1;
        }
    }
    return mapped_any ? 0 : 2;
}
