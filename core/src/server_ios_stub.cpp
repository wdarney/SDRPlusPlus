// Replacement for core/src/server.cpp on iOS. The full server.cpp implements
// SDR++'s "server mode" (a daemon that other SDR++ clients connect to over
// TCP and receive compressed IQ from). An iOS client never runs in server
// mode — `core::args["server"]` is never set — so the real implementation
// is dead code, and dragging it in pulls in the zstd compressor which is
// the only thing keeping libzstd in the build's compress path.
//
// We still need the public symbols to link, because they're called from
// core.cpp and signal_path/source.cpp behind branches that are statically
// dead at runtime but visible to the compiler. The stubs below satisfy the
// linker; if any of them ever fires, that's a programming error.

#include <server.h>
#include <utils/flog.h>

namespace server {
    void setInput(dsp::stream<dsp::complex_t>* /*stream*/) {
        // No-op: iOS client never owns the input that the server would publish.
    }

    int main() {
        flog::error("server::main() called on iOS build — server mode is not supported");
        return -1;
    }

    void setInputSampleRate(double /*samplerate*/) {
        // No-op: only meaningful in server mode.
    }
}
