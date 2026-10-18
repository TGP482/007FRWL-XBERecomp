#include "xdk/hle.h"

// Films do not hold the frame: the film update waited out each film frame, so menus over a film ran at the film's
// 25 fps. The update returns at once instead and draws the shown frame again until the next one is due.

using namespace kernel;

namespace {

// film player, the object at [0x77F954]
constexpr uint32_t kFilmMovie = 0x04;
constexpr uint32_t kFilmVideo = 0x08;      // video player, its Draw redraws the shown frame
constexpr uint32_t kFilmStopping = 0x22;   // byte, ends the wait
constexpr uint32_t kFilmAborted = 0x23;    // byte, ends the wait
constexpr uint32_t kFilmStartTime = 0x98;  // clock at the film's first frame

// thiscall on the movie: its clock passed the fetched frame, the test the wait loop spun on
constexpr uint32_t kMovieFrameDue = 0x437F40;

// the film whose fetched frame waits for its time
struct Pending {
    uint32_t movie = 0;
    uint32_t start = 0;
};
Pending g_pending;
bool g_holding = false;

bool Stopping(uint32_t film) { return G<uint8_t>(film + kFilmStopping) || G<uint8_t>(film + kFilmAborted); }

bool FrameDue(X86Context& ctx, uint32_t movie) {
    X86Context saved = ctx;
    ctx.ecx = movie;
    bool due = (CallGuest(ctx, kMovieFrameDue, {}) & 0xFF) != 0;
    ctx = saved;
    return due;
}

}  // namespace

// the wait for the fetched frame's time, 0x362D5F: skipped, later updates hold the shown frame instead
bool FilmSkipWait(X86Context& ctx, uint8_t*) {
    g_pending = {R32(ctx.esi + kFilmMovie), R32(ctx.esi + kFilmStartTime)};
    return true;
}

// before the update shows the fetched frame, 0x362CC5: until it is due, the redraw path
bool FilmHoldFrame(X86Context& ctx, uint8_t*) {
    uint32_t film = ctx.esi;
    uint32_t movie = R32(film + kFilmMovie);
    bool waiting = g_pending.movie == movie && g_pending.start == R32(film + kFilmStartTime);
    g_holding = waiting && !Stopping(film) && !FrameDue(ctx, movie);
    if (!g_holding) {
        g_pending = {};
        return false;
    }
    ctx.ecx = R32(film + kFilmVideo);
    return true;
}

// after the redraw, 0x362D05: a held update fetches nothing and returns
bool FilmHeld(X86Context&, uint8_t*) { return g_holding; }
