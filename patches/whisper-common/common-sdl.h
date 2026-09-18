#pragma once

#include <SDL.h>
#include <SDL_audio.h>

#include <atomic>
#include <cstdint>
#include <vector>
#include <mutex>

//
// SDL Audio capture
//

// r24.18: callback-published samples, in one monotonically increasing order.
// The span is half-open. These are SOFTWARE capture boundaries: SDL2 supplies
// no hardware timestamp with its callback, so they do not date acoustic onset.
// Existing get()/clear() keep their interfaces and behavior; callers opt into
// these receipts with their own independently restorable ATHENA_* policy.
struct audio_capture_range {
    uint64_t begin = 0;
    uint64_t end = 0;
};
// r24.20 review (VISION #2 / SPEECH F5 — law 10): this header is the overlay's
// copy of a STOCK whisper.cpp file, and the four cursor methods below exist
// only here. Code that calls them is compiled against whichever common-sdl.h
// the build finds first — the stock one after any `git checkout -f` of the
// whisper tree (stock install.sh does that whenever HEAD != pin). The macro
// lets talk-llama.cpp keep its r24.17 arms as the #else, so the brain builds
// either way; the templated consumers (the endpointer, OutageTape) detect the
// methods on the ring type instead and need nothing from here.
#define ATHENA_AUDIO_CURSOR 1

class audio_async {
public:
    audio_async(int len_ms);
    ~audio_async();

    bool init(int capture_id, int sample_rate);

    // start capturing audio via the provided SDL callback
    // keep last len_ms seconds of audio in a circular buffer
    bool resume();
    bool pause();
    bool clear();

    // callback to be called by SDL
    void callback(uint8_t * stream, int len);

    // get audio data from the circular buffer
    void get(int ms, std::vector<float> & audio);

    // Read a capture boundary or samples under the SAME mutex as callback().
    // clear()/pause()/resume() never reset this counter. A range clipped by
    // overwrite or clear starts after the requested position; callers can
    // therefore distinguish retained input from input the ring no longer has.
    uint64_t capture_position();
    audio_capture_range get_with_cursor(int ms, std::vector<float> & audio);
    audio_capture_range get_since(uint64_t from, std::vector<float> & audio,
                                  uint64_t through = UINT64_MAX);
    bool clear_through(uint64_t through);

private:
    SDL_AudioDeviceID m_dev_id_in = 0;

    int m_len_ms = 0;
    int m_sample_rate = 0;

    std::atomic_bool m_running;
    std::mutex       m_mutex;

    std::vector<float> m_audio;
    size_t             m_audio_pos = 0;
    size_t             m_audio_len = 0;
    uint64_t           m_audio_total = 0; // under m_mutex; counts callback input

    audio_capture_range get_range_locked(uint64_t from, uint64_t through,
                                          std::vector<float> & audio);
};

// Return false if need to quit
bool sdl_poll_events();
