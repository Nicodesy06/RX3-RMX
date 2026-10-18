/* SPDX-License-Identifier: MPL-2.0 */
/* Typed audio stages and the deck identity they are resolved against. The
 * callbacks run on the player's audio threads: this file adds no allocation,
 * I/O or waiting to those paths. Claims and releases run serially. */
#include "../api/rx3_audio_api.h"
#include "rx3_audio.h"
#include "rx3_hooks.h"
#include "rx3_log.h"

/* dsp::TimeStretch::getStreamAt is the deck's playback stream: it wraps
   PcmReader::getStreamAt and is the speed and master-tempo stage, so its
   output is what the deck plays. PcmReader::getStreamAt itself is shared with
   the BPM and waveform scan, which walks a track out of order; a sequential
   process cannot sit there. TimeStretch+4 is the reader, which names the deck. */
#define TIMESTRETCH_STREAM ((unsigned long)0x000c292c)
static const uint8_t stream_guard[8] = {0xf8,0x40,0x2d,0xe9,0x00,0x40,0xa0,0xe1};
/* The master bus, just before the microphone talkover attenuator. */
#define MIC_TALKOVER ((unsigned long)0x0009c750)
static const uint8_t master_guard[8] = {0x03,0xc0,0xd0,0xe5,0x04,0x40,0x2d,0xe5};

typedef unsigned long (*stream_fn)(void *, unsigned long, struct rx3_stereo *, unsigned long);
typedef void (*talkover_fn)(void *, void *, struct rx3_stereo *, int);

/* A stage's clients live in one of two tables. The audio thread reads the
   published one; a claim or release fills the other, publishes it with one
   store, then drains, so no callback can still be walking the table it will
   rewrite next time. Claims and releases run serially. */
struct client { const void *owner; void *function; unsigned int priority; };
struct table { unsigned int count; struct client entries[RX3_AUDIO_CLIENTS]; };
struct stage {
    struct installed_hook hook;
    void *original;
    struct table tables[2];
    struct table *volatile live;
    unsigned int active;
};
static struct stage deck_stage, master_stage;
static void *volatile deck_readers[2];

static void drain(struct stage *stage)
{
    for (;;) {
        while (__atomic_load_n(&stage->active, __ATOMIC_SEQ_CST)) usleep(1000u);
        usleep(10000u);
        if (!__atomic_load_n(&stage->active, __ATOMIC_SEQ_CST)) return;
    }
}

static const struct table *published(struct stage *stage)
{
    return __atomic_load_n(&stage->live, __ATOMIC_SEQ_CST);
}

static unsigned long stream_hooked(void *stretch, unsigned long position,
                                   struct rx3_stereo *output, unsigned long frames)
{
    __atomic_add_fetch(&deck_stage.active, 1u, __ATOMIC_SEQ_CST);
    unsigned long result = ((stream_fn)deck_stage.original)(stretch, position, output, frames);
    const struct table *table = published(&deck_stage);
    const void *reader = *(void **)((uint8_t *)stretch + 4u);
    int deck = rx3_audio_deck_for_reader(reader);
    if (table && output && frames && deck >= 0)
        for (unsigned int i = 0; i < table->count; i++)
            ((rx3_deck_stream_fn)table->entries[i].function)((unsigned int)deck, reader,
                                                             (int)position, output,
                                                             (unsigned int)frames);
    __atomic_sub_fetch(&deck_stage.active, 1u, __ATOMIC_SEQ_CST);
    return result;
}

static void master_hooked(void *self, void *state, struct rx3_stereo *output, int frames)
{
    __atomic_add_fetch(&master_stage.active, 1u, __ATOMIC_SEQ_CST);
    const struct table *table = published(&master_stage);
    if (table && output && frames > 0)
        for (unsigned int i = 0; i < table->count; i++)
            ((rx3_master_fn)table->entries[i].function)(output, (unsigned int)frames);
    ((talkover_fn)master_stage.original)(self, state, output, frames);
    __atomic_sub_fetch(&master_stage.active, 1u, __ATOMIC_SEQ_CST);
}

/* The table that is not live, to be filled and published. */
static struct table *spare(struct stage *stage)
{
    const struct table *live = published(stage);
    return live == &stage->tables[0] ? &stage->tables[1] : &stage->tables[0];
}

static unsigned int client_count(struct stage *stage)
{
    const struct table *live = published(stage);
    return live ? live->count : 0u;
}

static void publish(struct stage *stage, struct table *next)
{
    __atomic_store_n(&stage->live, next->count ? next : 0, __ATOMIC_SEQ_CST);
    drain(stage);
}

static int claim(struct stage *stage, const void *owner, unsigned int priority, void *client,
                 unsigned long address, const uint8_t guard[8], void *replacement)
{
    if (!owner || !client) return 0;
    const struct table *live = published(stage);
    unsigned int count = live ? live->count : 0u;
    if (count >= RX3_AUDIO_CLIENTS) return 0;
    for (unsigned int i = 0; i < count; i++)
        if (live->entries[i].owner == owner) return 0;
    if (!stage->original) {
        stage->original = install_hook(&stage->hook, address, guard, replacement);
        if (!stage->original) return 0;
    }
    /* Ascending priority; an equal one goes after those already there. */
    struct table *next = spare(stage);
    unsigned int i = 0u, n = 0u;
    while (i < count && live->entries[i].priority <= priority) next->entries[n++] = live->entries[i++];
    next->entries[n++] = (struct client){owner, client, priority};
    while (i < count) next->entries[n++] = live->entries[i++];
    next->count = n;
    publish(stage, next);
    return 1;
}

/* Take the client out, drain, and with the last client detach and release
   the trampoline. A failed restore keeps the replacement reachable, passing
   audio through untouched. */
static void release(struct stage *stage, const void *owner)
{
    const struct table *live = published(stage);
    if (!owner || !live) return;
    struct table *next = spare(stage);
    unsigned int n = 0u, found = 0u;
    for (unsigned int i = 0; i < live->count; i++) {
        if (live->entries[i].owner == owner) { found = 1u; continue; }
        next->entries[n++] = live->entries[i];
    }
    if (!found) return;
    next->count = n;
    publish(stage, next);
    if (n || !stage->original) return;
    int detached = detach_hook(&stage->hook);
    drain(stage);
    if (!detached) {
        log_line("audio: native stage retained after restore failure");
        return;
    }
    (void)release_hook(&stage->hook);
    stage->original = 0;
}

static int claim_deck_stream_at(const void *owner, unsigned int priority, rx3_deck_stream_fn client)
{
    return claim(&deck_stage, owner, priority, (void *)client, TIMESTRETCH_STREAM, stream_guard,
                 (void *)stream_hooked);
}
static int claim_deck_stream(const void *owner, rx3_deck_stream_fn client)
{
    return claim_deck_stream_at(owner, RX3_AUDIO_PRIORITY_DEFAULT, client);
}
static void release_deck_stream(const void *owner) { release(&deck_stage, owner); }
static int claim_master_at(const void *owner, unsigned int priority, rx3_master_fn client)
{
    return claim(&master_stage, owner, priority, (void *)client, MIC_TALKOVER, master_guard,
                 (void *)master_hooked);
}
static int claim_master(const void *owner, rx3_master_fn client)
{
    return claim_master_at(owner, RX3_AUDIO_PRIORITY_DEFAULT, client);
}
static void release_master(const void *owner) { release(&master_stage, owner); }

/* PcmReader::load runs on the native loading thread. The old reader stops
   naming a deck before the load; the new one only after it. */
void rx3_audio_track_loading(unsigned int deck)
{
    if (deck < 2u) __atomic_store_n(&deck_readers[deck], 0, __ATOMIC_SEQ_CST);
}
void rx3_audio_track_loaded(unsigned int deck, void *reader)
{
    if (deck < 2u) __atomic_store_n(&deck_readers[deck], reader, __ATOMIC_SEQ_CST);
}
int rx3_audio_deck_for_reader(const void *reader)
{
    for (unsigned int deck = 0; deck < 2u; deck++)
        if (reader && __atomic_load_n(&deck_readers[deck], __ATOMIC_SEQ_CST) == reader)
            return (int)deck;
    return -1;
}
unsigned int rx3_audio_count(void)
{
    return client_count(&deck_stage) + client_count(&master_stage);
}

const struct rx3_audio_service rx3_audio = {
    claim_deck_stream, release_deck_stream, claim_master, release_master,
    claim_deck_stream_at, claim_master_at
};
