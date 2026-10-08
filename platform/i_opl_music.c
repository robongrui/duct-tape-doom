/* OPL FM music. DOOM's MUS and MIDI scores play through an emulated
   Yamaha OPL3 with the WAD's GENMIDI instruments, the way DOS DOOM
   sounded on AdLib and Sound Blaster cards. Voice allocation, pitch and
   volume follow the DMX 1.9 library as reconstructed by Chocolate Doom
   (src/i_oplmusic.c, GPL-2+, Simon Howard), whose tables are used below. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "doomdef.h"
#include "i_system.h"
#include "m_argv.h"
#include "w_wad.h"
#include "z_zone.h"
#include "i_opl_music.h"
#include "opl3.h"

#define NUM_VOICES		18	/* OPL3: two banks of nine */
#define NUM_CHANNELS		16
#define PERCUSSION		15	/* DMX works in MUS channel numbers */
#define GENMIDI_HEADER		"#OPL_II#"
#define GENMIDI_INSTRUMENTS	128
#define GENMIDI_PERCUSSION	47	/* keys 35-81 */
#define GENMIDI_RECORD		36
#define GENMIDI_FIXED		0x0001
#define GENMIDI_DOUBLE		0x0004
#define MUS_RATE		140	/* MUS ticks per second */
#define OUTPUT_GAIN		1.5f	/* DOOM's scores peak near 0.3 of full scale */

enum { EV_NOTE_OFF, EV_NOTE_ON, EV_CONTROLLER, EV_PROGRAM, EV_BEND };
enum { CTRL_VOLUME = 7, CTRL_PAN = 10, CTRL_SOUNDS_OFF = 120,
       CTRL_RESET = 121, CTRL_NOTES_OFF = 123 };

typedef struct { byte tremolo, attack, sustain, waveform, scale, level; } opl_operator_t;
typedef struct
{
    opl_operator_t	modulator, carrier;
    byte		feedback;
    int			base_note;
} opl_layer_t;
typedef struct
{
    int			flags, fine_tuning, fixed_note;
    opl_layer_t		layers[2];
} instrument_t;

typedef struct { uint32_t frame; byte type, channel, data1, data2; } score_event_t;

typedef struct
{
    const instrument_t*	instrument;
    int			volume;
    int			pan;	/* output bits for register 0xc0 */
    int			bend;	/* 1/32 semitones */
} channel_t;

typedef struct
{
    int			index, op1, op2, bank;
    const instrument_t*	instrument;
    int			layer;
    channel_t*		channel;
    int			key, note, note_volume;
    unsigned		freq, car_volume, mod_volume, pan;
} voice_t;

static const unsigned short frequency_curve[] = {

    0x133, 0x133, 0x134, 0x134, 0x135, 0x136, 0x136, 0x137,   // -1
    0x137, 0x138, 0x138, 0x139, 0x139, 0x13a, 0x13b, 0x13b,
    0x13c, 0x13c, 0x13d, 0x13d, 0x13e, 0x13f, 0x13f, 0x140,
    0x140, 0x141, 0x142, 0x142, 0x143, 0x143, 0x144, 0x144,

    0x145, 0x146, 0x146, 0x147, 0x147, 0x148, 0x149, 0x149,   // -2
    0x14a, 0x14a, 0x14b, 0x14c, 0x14c, 0x14d, 0x14d, 0x14e,
    0x14f, 0x14f, 0x150, 0x150, 0x151, 0x152, 0x152, 0x153,
    0x153, 0x154, 0x155, 0x155, 0x156, 0x157, 0x157, 0x158,

    // These are used for the first seven MIDI note values:

    0x158, 0x159, 0x15a, 0x15a, 0x15b, 0x15b, 0x15c, 0x15d,   // 0
    0x15d, 0x15e, 0x15f, 0x15f, 0x160, 0x161, 0x161, 0x162,
    0x162, 0x163, 0x164, 0x164, 0x165, 0x166, 0x166, 0x167,
    0x168, 0x168, 0x169, 0x16a, 0x16a, 0x16b, 0x16c, 0x16c,

    0x16d, 0x16e, 0x16e, 0x16f, 0x170, 0x170, 0x171, 0x172,   // 1
    0x172, 0x173, 0x174, 0x174, 0x175, 0x176, 0x176, 0x177,
    0x178, 0x178, 0x179, 0x17a, 0x17a, 0x17b, 0x17c, 0x17c,
    0x17d, 0x17e, 0x17e, 0x17f, 0x180, 0x181, 0x181, 0x182,

    0x183, 0x183, 0x184, 0x185, 0x185, 0x186, 0x187, 0x188,   // 2
    0x188, 0x189, 0x18a, 0x18a, 0x18b, 0x18c, 0x18d, 0x18d,
    0x18e, 0x18f, 0x18f, 0x190, 0x191, 0x192, 0x192, 0x193,
    0x194, 0x194, 0x195, 0x196, 0x197, 0x197, 0x198, 0x199,

    0x19a, 0x19a, 0x19b, 0x19c, 0x19d, 0x19d, 0x19e, 0x19f,   // 3
    0x1a0, 0x1a0, 0x1a1, 0x1a2, 0x1a3, 0x1a3, 0x1a4, 0x1a5,
    0x1a6, 0x1a6, 0x1a7, 0x1a8, 0x1a9, 0x1a9, 0x1aa, 0x1ab,
    0x1ac, 0x1ad, 0x1ad, 0x1ae, 0x1af, 0x1b0, 0x1b0, 0x1b1,

    0x1b2, 0x1b3, 0x1b4, 0x1b4, 0x1b5, 0x1b6, 0x1b7, 0x1b8,   // 4
    0x1b8, 0x1b9, 0x1ba, 0x1bb, 0x1bc, 0x1bc, 0x1bd, 0x1be,
    0x1bf, 0x1c0, 0x1c0, 0x1c1, 0x1c2, 0x1c3, 0x1c4, 0x1c4,
    0x1c5, 0x1c6, 0x1c7, 0x1c8, 0x1c9, 0x1c9, 0x1ca, 0x1cb,

    0x1cc, 0x1cd, 0x1ce, 0x1ce, 0x1cf, 0x1d0, 0x1d1, 0x1d2,   // 5
    0x1d3, 0x1d3, 0x1d4, 0x1d5, 0x1d6, 0x1d7, 0x1d8, 0x1d8,
    0x1d9, 0x1da, 0x1db, 0x1dc, 0x1dd, 0x1de, 0x1de, 0x1df,
    0x1e0, 0x1e1, 0x1e2, 0x1e3, 0x1e4, 0x1e5, 0x1e5, 0x1e6,

    0x1e7, 0x1e8, 0x1e9, 0x1ea, 0x1eb, 0x1ec, 0x1ed, 0x1ed,   // 6
    0x1ee, 0x1ef, 0x1f0, 0x1f1, 0x1f2, 0x1f3, 0x1f4, 0x1f5,
    0x1f6, 0x1f6, 0x1f7, 0x1f8, 0x1f9, 0x1fa, 0x1fb, 0x1fc,
    0x1fd, 0x1fe, 0x1ff, 0x200, 0x201, 0x201, 0x202, 0x203,

    // First note of looped range used for all octaves:

    0x204, 0x205, 0x206, 0x207, 0x208, 0x209, 0x20a, 0x20b,   // 7
    0x20c, 0x20d, 0x20e, 0x20f, 0x210, 0x210, 0x211, 0x212,
    0x213, 0x214, 0x215, 0x216, 0x217, 0x218, 0x219, 0x21a,
    0x21b, 0x21c, 0x21d, 0x21e, 0x21f, 0x220, 0x221, 0x222,

    0x223, 0x224, 0x225, 0x226, 0x227, 0x228, 0x229, 0x22a,   // 8
    0x22b, 0x22c, 0x22d, 0x22e, 0x22f, 0x230, 0x231, 0x232,
    0x233, 0x234, 0x235, 0x236, 0x237, 0x238, 0x239, 0x23a,
    0x23b, 0x23c, 0x23d, 0x23e, 0x23f, 0x240, 0x241, 0x242,

    0x244, 0x245, 0x246, 0x247, 0x248, 0x249, 0x24a, 0x24b,   // 9
    0x24c, 0x24d, 0x24e, 0x24f, 0x250, 0x251, 0x252, 0x253,
    0x254, 0x256, 0x257, 0x258, 0x259, 0x25a, 0x25b, 0x25c,
    0x25d, 0x25e, 0x25f, 0x260, 0x262, 0x263, 0x264, 0x265,

    0x266, 0x267, 0x268, 0x269, 0x26a, 0x26c, 0x26d, 0x26e,   // 10
    0x26f, 0x270, 0x271, 0x272, 0x273, 0x275, 0x276, 0x277,
    0x278, 0x279, 0x27a, 0x27b, 0x27d, 0x27e, 0x27f, 0x280,
    0x281, 0x282, 0x284, 0x285, 0x286, 0x287, 0x288, 0x289,

    0x28b, 0x28c, 0x28d, 0x28e, 0x28f, 0x290, 0x292, 0x293,   // 11
    0x294, 0x295, 0x296, 0x298, 0x299, 0x29a, 0x29b, 0x29c,
    0x29e, 0x29f, 0x2a0, 0x2a1, 0x2a2, 0x2a4, 0x2a5, 0x2a6,
    0x2a7, 0x2a9, 0x2aa, 0x2ab, 0x2ac, 0x2ae, 0x2af, 0x2b0,

    0x2b1, 0x2b2, 0x2b4, 0x2b5, 0x2b6, 0x2b7, 0x2b9, 0x2ba,   // 12
    0x2bb, 0x2bd, 0x2be, 0x2bf, 0x2c0, 0x2c2, 0x2c3, 0x2c4,
    0x2c5, 0x2c7, 0x2c8, 0x2c9, 0x2cb, 0x2cc, 0x2cd, 0x2ce,
    0x2d0, 0x2d1, 0x2d2, 0x2d4, 0x2d5, 0x2d6, 0x2d8, 0x2d9,

    0x2da, 0x2dc, 0x2dd, 0x2de, 0x2e0, 0x2e1, 0x2e2, 0x2e4,   // 13
    0x2e5, 0x2e6, 0x2e8, 0x2e9, 0x2ea, 0x2ec, 0x2ed, 0x2ee,
    0x2f0, 0x2f1, 0x2f2, 0x2f4, 0x2f5, 0x2f6, 0x2f8, 0x2f9,
    0x2fb, 0x2fc, 0x2fd, 0x2ff, 0x300, 0x302, 0x303, 0x304,

    0x306, 0x307, 0x309, 0x30a, 0x30b, 0x30d, 0x30e, 0x310,   // 14
    0x311, 0x312, 0x314, 0x315, 0x317, 0x318, 0x31a, 0x31b,
    0x31c, 0x31e, 0x31f, 0x321, 0x322, 0x324, 0x325, 0x327,
    0x328, 0x329, 0x32b, 0x32c, 0x32e, 0x32f, 0x331, 0x332,

    0x334, 0x335, 0x337, 0x338, 0x33a, 0x33b, 0x33d, 0x33e,   // 15
    0x340, 0x341, 0x343, 0x344, 0x346, 0x347, 0x349, 0x34a,
    0x34c, 0x34d, 0x34f, 0x350, 0x352, 0x353, 0x355, 0x357,
    0x358, 0x35a, 0x35b, 0x35d, 0x35e, 0x360, 0x361, 0x363,

    0x365, 0x366, 0x368, 0x369, 0x36b, 0x36c, 0x36e, 0x370,   // 16
    0x371, 0x373, 0x374, 0x376, 0x378, 0x379, 0x37b, 0x37c,
    0x37e, 0x380, 0x381, 0x383, 0x384, 0x386, 0x388, 0x389,
    0x38b, 0x38d, 0x38e, 0x390, 0x392, 0x393, 0x395, 0x397,

    0x398, 0x39a, 0x39c, 0x39d, 0x39f, 0x3a1, 0x3a2, 0x3a4,   // 17
    0x3a6, 0x3a7, 0x3a9, 0x3ab, 0x3ac, 0x3ae, 0x3b0, 0x3b1,
    0x3b3, 0x3b5, 0x3b7, 0x3b8, 0x3ba, 0x3bc, 0x3bd, 0x3bf,
    0x3c1, 0x3c3, 0x3c4, 0x3c6, 0x3c8, 0x3ca, 0x3cb, 0x3cd,

    // The last note has an incomplete range, and loops round back to
    // the start.  Note that the last value is actually a buffer overrun
    // and does not fit with the other values.

    0x3cf, 0x3d1, 0x3d2, 0x3d4, 0x3d6, 0x3d8, 0x3da, 0x3db,   // 18
    0x3dd, 0x3df, 0x3e1, 0x3e3, 0x3e4, 0x3e6, 0x3e8, 0x3ea,
    0x3ec, 0x3ed, 0x3ef, 0x3f1, 0x3f3, 0x3f5, 0x3f6, 0x3f8,
    0x3fa, 0x3fc, 0x3fe, 0x36c,
};

static const unsigned int volume_mapping_table[] = {
    0, 1, 3, 5, 6, 8, 10, 11,
    13, 14, 16, 17, 19, 20, 22, 23,
    25, 26, 27, 29, 30, 32, 33, 34,
    36, 37, 39, 41, 43, 45, 47, 49,
    50, 52, 54, 55, 57, 59, 60, 61,
    63, 64, 66, 67, 68, 69, 71, 72,
    73, 74, 75, 76, 77, 79, 80, 81,
    82, 83, 84, 84, 85, 86, 87, 88,
    89, 90, 91, 92, 92, 93, 94, 95,
    96, 96, 97, 98, 99, 99, 100, 101,
    101, 102, 103, 103, 104, 105, 105, 106,
    107, 107, 108, 109, 109, 110, 110, 111,
    112, 112, 113, 113, 114, 114, 115, 115,
    116, 117, 117, 118, 118, 119, 119, 120,
    120, 121, 121, 122, 122, 123, 123, 123,
    124, 124, 125, 125, 126, 126, 127, 127
};

static const int voice_operators[2][9] =
{
    { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12 },
    { 0x03, 0x04, 0x05, 0x0b, 0x0c, 0x0d, 0x13, 0x14, 0x15 }
};

static opl3_chip	chip;
static int		output_rate;
static boolean		ready, failed;
static instrument_t	instruments[GENMIDI_INSTRUMENTS + GENMIDI_PERCUSSION];
static channel_t	channels[NUM_CHANNELS];
static voice_t		voices[NUM_VOICES];
static voice_t*		free_voices[NUM_VOICES];
static voice_t*		used_voices[NUM_VOICES];
static int		num_free, num_used;

static score_event_t*		score;
static int		num_score, max_score;
static int		next_event;
static uint32_t		song_frame, song_length;
static boolean		song_loaded, playing, looping, paused;
static float		music_gain = OUTPUT_GAIN;

static void write_reg (unsigned reg, unsigned value)
{
    OPL3_WriteReg (&chip, (uint16_t)reg, (uint8_t)value);
}

/* DMX's register setup, including its writes to registers that do not exist. */
static void init_registers (void)
{
    for (unsigned bank = 0; bank <= 0x100; bank += 0x100)
    {
	for (unsigned r = 0x40; r <= 0x40 + 0x15; ++r) write_reg (bank | r, 0x3f);
	for (unsigned r = 0x60; r <= 0xe0 + 0x15; ++r) write_reg (bank | r, 0);
	for (unsigned r = 1; r < 0x40; ++r) write_reg (bank | r, 0);
	if (!bank)
	{
	    write_reg (0x04, 0x60);
	    write_reg (0x04, 0x80);
	    write_reg (0x01, 0x20);
	    write_reg (0x105, 0x01);
	}
    }
    write_reg (0x08, 0x40);
    write_reg (0x105, 0x01);
}

static void read_operator (const byte* p, opl_operator_t* op)
{
    op->tremolo = p[0]; op->attack = p[1]; op->sustain = p[2];
    op->waveform = p[3]; op->scale = p[4]; op->level = p[5];
}

static boolean load_instruments (void)
{
    int lump = W_CheckNumForName ("GENMIDI");
    int size = GENMIDI_INSTRUMENTS + GENMIDI_PERCUSSION;
    const byte* data;

    if (lump < 0 || W_LumpLength (lump) < 8 + size * GENMIDI_RECORD)
    {
	fprintf (stderr, "OPL music needs a GENMIDI lump; music disabled.\n");
	return false;
    }
    data = (const byte*)W_CacheLumpNum (lump, PU_STATIC) + 8;
    for (int i = 0; i < size; ++i, data += GENMIDI_RECORD)
    {
	instrument_t* instrument = &instruments[i];
	instrument->flags = data[0] | data[1] << 8;
	instrument->fine_tuning = data[2];
	instrument->fixed_note = data[3];
	for (int l = 0; l < 2; ++l)
	{
	    const byte* layer = data + 4 + 16 * l;
	    read_operator (layer, &instrument->layers[l].modulator);
	    instrument->layers[l].feedback = layer[6];
	    read_operator (layer + 7, &instrument->layers[l].carrier);
	    instrument->layers[l].base_note = (int16_t)(layer[14] | layer[15] << 8);
	}
    }
    return true;
}

static void init_voices (void)
{
    num_free = NUM_VOICES;
    num_used = 0;
    for (int i = 0; i < NUM_VOICES; ++i)
    {
	voices[i] = (voice_t){ .index = i % 9, .op1 = voice_operators[0][i % 9],
			       .op2 = voice_operators[1][i % 9], .bank = (i / 9) << 8 };
	free_voices[i] = &voices[i];
    }
}

static void init_channel (channel_t* channel)
{
    channel->instrument = &instruments[0];
    channel->volume = 100;
    channel->pan = 0x30;
    channel->bend = 0;
}

static void load_operator (int op, const opl_operator_t* data, boolean silent, unsigned* volume)
{
    *volume = data->scale | (silent ? 0x3f : data->level);
    write_reg (0x40 + op, *volume);
    write_reg (0x20 + op, data->tremolo);
    write_reg (0x60 + op, data->attack);
    write_reg (0x80 + op, data->sustain);
    write_reg (0xe0 + op, data->waveform);
}

static void set_instrument (voice_t* voice, const instrument_t* instrument, int layer)
{
    const opl_layer_t* data = &instrument->layers[layer];
    boolean modulating = (data->feedback & 1) == 0;

    if (voice->instrument == instrument && voice->layer == layer)
	return;
    voice->instrument = instrument;
    voice->layer = layer;
    /* The carrier stays silent until set_volume; so does the modulator
       when both operators sound. */
    load_operator (voice->op2 | voice->bank, &data->carrier, true, &voice->car_volume);
    load_operator (voice->op1 | voice->bank, &data->modulator, !modulating, &voice->mod_volume);
    write_reg ((0xc0 + voice->index) | voice->bank, data->feedback | voice->pan);
}

static void set_volume (voice_t* voice, int volume)
{
    const opl_layer_t* data = &voice->instrument->layers[voice->layer];
    unsigned channel_volume = 2 * (volume_mapping_table[voice->channel->volume] + 1);
    unsigned full = (volume_mapping_table[volume] * channel_volume) >> 9;
    unsigned car = 0x3f - full;

    voice->note_volume = volume;
    if (car == (voice->car_volume & 0x3f))
	return;
    voice->car_volume = car | (voice->car_volume & 0xc0);
    write_reg ((0x40 + voice->op2) | voice->bank, voice->car_volume);
    if ((data->feedback & 1) && data->modulator.level != 0x3f)
    {
	unsigned mod = data->modulator.level < car ? car : data->modulator.level;
	mod |= voice->mod_volume & 0xc0;
	if (mod != voice->mod_volume)
	{
	    voice->mod_volume = mod;
	    write_reg ((0x40 + voice->op1) | voice->bank, mod | (data->modulator.scale & 0xc0));
	}
    }
}

static unsigned voice_frequency (const voice_t* voice)
{
    const instrument_t* instrument = voice->instrument;
    int note = voice->note;
    int index;

    if (!(instrument->flags & GENMIDI_FIXED))
	note += instrument->layers[voice->layer].base_note;
    while (note < 0) note += 12;
    while (note > 95) note -= 12;
    index = 64 + 32 * note + voice->channel->bend;
    if (voice->layer)
	index += instrument->fine_tuning / 2 - 64;
    if (index < 0)
	index = 0;
    if (index < 284)
	return frequency_curve[index];
    {
	unsigned octave = (unsigned)(index - 284) / (12 * 32);
	return frequency_curve[(index - 284) % (12 * 32) + 284] | (octave > 7 ? 7 : octave) << 10;
    }
}

static void update_frequency (voice_t* voice)
{
    unsigned freq = voice_frequency (voice);

    if (voice->freq == freq)
	return;
    write_reg ((0xa0 + voice->index) | voice->bank, freq & 0xff);
    write_reg ((0xb0 + voice->index) | voice->bank, (freq >> 8) | 0x20);
    voice->freq = freq;
}

static void release_voice (int slot)
{
    voice_t* voice = used_voices[slot];

    write_reg ((0xb0 + voice->index) | voice->bank, voice->freq >> 8);
    voice->channel = NULL;
    voice->note = 0;
    --num_used;
    memmove (&used_voices[slot], &used_voices[slot + 1], (num_used - slot) * sizeof(*used_voices));
    free_voices[num_free++] = voice;
}

/* With every voice busy, drop a second layer or the newest voice on the
   highest channel; low channels win, as in DMX. */
static void replace_voice (void)
{
    int result = 0;

    for (int i = 0; i < num_used; ++i)
	if (used_voices[i]->layer || used_voices[i]->channel >= used_voices[result]->channel)
	    result = i;
    release_voice (result);
}

static void voice_on (channel_t* channel, const instrument_t* instrument, int layer,
		      int note, int key, int volume)
{
    voice_t* voice;

    if (!num_free)
	return;
    voice = free_voices[0];
    memmove (&free_voices[0], &free_voices[1], --num_free * sizeof(*free_voices));
    used_voices[num_used++] = voice;
    voice->channel = channel;
    voice->key = key;
    voice->note = (instrument->flags & GENMIDI_FIXED) ? instrument->fixed_note : note;
    voice->pan = channel->pan;
    set_instrument (voice, instrument, layer);
    set_volume (voice, volume);
    voice->freq = 0;
    update_frequency (voice);
}

static void note_off (channel_t* channel, int key)
{
    for (int i = 0; i < num_used; ++i)
	if (used_voices[i]->channel == channel && used_voices[i]->key == key)
	    release_voice (i--);
}

static void notes_off (channel_t* channel)
{
    for (int i = 0; i < num_used; ++i)
	if (used_voices[i]->channel == channel)
	    release_voice (i--);
}

static void note_on (int number, int key, int volume)
{
    channel_t* channel = &channels[number];
    const instrument_t* instrument = channel->instrument;
    int note = key;

    if (!volume)
    {
	note_off (channel, key);
	return;
    }
    if (number == PERCUSSION)
    {
	if (key < 35 || key > 81)
	    return;
	instrument = &instruments[GENMIDI_INSTRUMENTS + key - 35];
	note = 60;
    }
    if (!num_free)
	replace_voice ();
    voice_on (channel, instrument, 0, note, key, volume);
    if (instrument->flags & GENMIDI_DOUBLE)
	voice_on (channel, instrument, 1, note, key, volume);
}

static void controller (int number, int control, int value)
{
    channel_t* channel = &channels[number];

    switch (control)
    {
      case CTRL_VOLUME:
	channel->volume = value;
	for (int i = 0; i < num_used; ++i)
	    if (used_voices[i]->channel == channel)
		set_volume (used_voices[i], used_voices[i]->note_volume);
	break;
      case CTRL_PAN:
	{
	    /* OPL3 output bits 0x10 and 0x20 are the left and right speakers. */
	    int pan = value <= 48 ? 0x10 : value >= 96 ? 0x20 : 0x30;
	    if (pan == channel->pan)
		break;
	    channel->pan = pan;
	    for (int i = 0; i < num_used; ++i)
		if (used_voices[i]->channel == channel)
		{
		    voice_t* voice = used_voices[i];
		    voice->pan = pan;
		    write_reg ((0xc0 + voice->index) | voice->bank,
			       voice->instrument->layers[voice->layer].feedback | pan);
		}
	}
	break;
      case CTRL_SOUNDS_OFF:
      case CTRL_NOTES_OFF:
	notes_off (channel);
	break;
      case CTRL_RESET:
	channel->volume = 100;
	channel->bend = 0;
	break;
      default:
	break;
    }
}

static void pitch_bend (int number, int value)
{
    channel_t* channel = &channels[number];
    voice_t* bent[NUM_VOICES];
    int count = 0, kept = 0;

    /* DMX bends by the most significant seven bits: two semitones each way.
       Bent voices move to the end of the list, so they are replaced last. */
    channel->bend = value - 64;
    for (int i = 0; i < num_used; ++i)
    {
	if (used_voices[i]->channel == channel)
	{
	    update_frequency (used_voices[i]);
	    bent[count++] = used_voices[i];
	}
	else
	    used_voices[kept++] = used_voices[i];
    }
    memcpy (&used_voices[kept], bent, count * sizeof(*bent));
}

static void process (const score_event_t* event)
{
    switch (event->type)
    {
      case EV_NOTE_OFF: note_off (&channels[event->channel], event->data1); break;
      case EV_NOTE_ON: note_on (event->channel, event->data1, event->data2); break;
      case EV_CONTROLLER: controller (event->channel, event->data1, event->data2); break;
      case EV_PROGRAM: channels[event->channel].instrument = &instruments[event->data1 & 127]; break;
      case EV_BEND: pitch_bend (event->channel, event->data2); break;
    }
}

static void reset_song (void)
{
    while (num_used)
	release_voice (0);
    for (int i = 0; i < NUM_CHANNELS; ++i)
	init_channel (&channels[i]);
    next_event = 0;
    song_frame = 0;
}

/*
 * Score loading. Both formats become one list of score in MUS channel
 * numbers, timed in output frames.
 */
static void add_event (double seconds, int type, int channel, int data1, int data2)
{
    if (num_score == max_score)
    {
	max_score = max_score ? max_score * 2 : 1024;
	score = realloc (score, max_score * sizeof(*score));
	if (!score)
	    I_Error ("OPL music: out of memory");
    }
    score[num_score++] = (score_event_t){ (uint32_t)(seconds * output_rate + 0.5), (byte)type,
				      (byte)channel, (byte)(data1 & 127), (byte)(data2 & 127) };
}

static boolean load_mus (const byte* data, int size)
{
    static const byte controls[] = { 0, 0, 1, CTRL_VOLUME, CTRL_PAN, 11, 91, 93, 64, 67 };
    static const byte system[] = { CTRL_SOUNDS_OFF, CTRL_NOTES_OFF, 126, 127, CTRL_RESET };
    byte velocity[NUM_CHANNELS];
    unsigned start, length;
    const byte *p, *end;
    double ticks = 0;

    if (size < 16 || memcmp (data, "MUS\x1a", 4))
	return false;
    length = data[4] | data[5] << 8;
    start = data[6] | data[7] << 8;
    if (start > (unsigned)size || length > (unsigned)size - start)
	return false;
    for (int c = 0; c < NUM_CHANNELS; ++c)
	velocity[c] = 127;
    p = data + start;
    end = p + length;
    while (p < end)
    {
	byte descriptor = *p++;
	int channel = descriptor & 15;
	int type = (descriptor >> 4) & 7;
	double seconds = ticks / MUS_RATE;

	if (type == 6)
	    break;
	if (type != 5 && p == end)
	    return false;
	switch (type)
	{
	  case 0:
	    add_event (seconds, EV_NOTE_OFF, channel, *p++, 0);
	    break;
	  case 1:
	    {
		byte note = *p++;
		if (note & 128)
		{
		    if (p == end)
			return false;
		    velocity[channel] = *p++ & 127;
		}
		add_event (seconds, EV_NOTE_ON, channel, note, velocity[channel]);
	    }
	    break;
	  case 2:
	    add_event (seconds, EV_BEND, channel, 0, *p++ >> 1);
	    break;
	  case 3:
	    {
		byte number = *p++;
		if (number >= 10 && number <= 14)
		    add_event (seconds, EV_CONTROLLER, channel, system[number - 10], 0);
	    }
	    break;
	  case 4:
	    {
		byte number = *p++, value;
		if (p == end)
		    return false;
		value = *p++;
		if (value > 127)
		    value = 127;
		if (number == 0)
		    add_event (seconds, EV_PROGRAM, channel, value, 0);
		else if (number < 10)
		    add_event (seconds, EV_CONTROLLER, channel, controls[number], value);
	    }
	    break;
	  case 7:
	    p++;
	    break;
	}
	if (descriptor & 128)
	{
	    uint32_t delay = 0;
	    byte next;
	    do
	    {
		if (p == end)
		    return false;
		next = *p++;
		delay = delay << 7 | (next & 127);
	    } while (next & 128);
	    ticks += delay;
	}
    }
    song_length = (uint32_t)(ticks / MUS_RATE * output_rate);
    return true;
}

typedef struct { uint32_t tick; int order; byte status, data1, data2; uint32_t tempo; } midi_event_t;

static int compare_midi (const void* a, const void* b)
{
    const midi_event_t* x = a;
    const midi_event_t* y = b;
    if (x->tick != y->tick)
	return x->tick < y->tick ? -1 : 1;
    return x->order - y->order;
}

static uint32_t read_be (const byte* p, int bytes)
{
    uint32_t value = 0;
    while (bytes--)
	value = value << 8 | *p++;
    return value;
}

static boolean read_varlen (const byte** p, const byte* end, uint32_t* value)
{
    *value = 0;
    for (int i = 0; i < 4; ++i)
    {
	if (*p >= end)
	    return false;
	*value = *value << 7 | (**p & 127);
	if (!(*(*p)++ & 128))
	    return true;
    }
    return false;
}

/* Standard MIDI files: all tracks merge onto one tempo map. A damaged
   track ends where the damage starts. */
static boolean load_midi (const byte* data, int size)
{
    midi_event_t* list = NULL;
    int count = 0, capacity = 0;
    unsigned tracks, division;
    const byte* p;
    const byte* end = data + size;

    if (size < 14 || read_be (data + 4, 4) < 6)
	return false;
    tracks = read_be (data + 10, 2);
    division = read_be (data + 12, 2);
    p = data + 8 + read_be (data + 4, 4);
    if (!division || p > end)
	return false;
    for (unsigned t = 0; t < tracks && p + 8 <= end; ++t)
    {
	uint32_t length = read_be (p + 4, 4);
	const byte* q = p + 8;
	const byte* track_end = q + length;
	uint32_t tick = 0;
	byte status = 0;

	if (memcmp (p, "MTrk", 4) || length > (uint32_t)(end - q))
	    break;
	p = track_end;
	while (q < track_end)
	{
	    uint32_t delta;
	    midi_event_t event = { 0 };

	    if (!read_varlen (&q, track_end, &delta) || q >= track_end)
		break;
	    tick += delta;
	    if (*q & 128)
		status = *q++;
	    if (status == 0xff || status == 0xf0 || status == 0xf7)
	    {
		byte meta = status == 0xff && q < track_end ? *q++ : 0;
		uint32_t skip;
		if (!read_varlen (&q, track_end, &skip) || skip > (uint32_t)(track_end - q))
		    break;
		if (status == 0xff && meta == 0x51 && skip == 3)
		{
		    event.tempo = read_be (q, 3);
		    event.status = 0xff;
		}
		q += skip;
		status = 0;	/* system messages cancel running status */
		if (meta == 0x2f)
		    break;
		if (!event.status)
		    continue;
	    }
	    else
	    {
		int type = status >> 4;
		int bytes = type == 0xc || type == 0xd ? 1 : 2;
		if (type < 8 || q + bytes > track_end)
		    break;
		event.status = status;
		event.data1 = q[0];
		event.data2 = bytes == 2 ? q[1] : 0;
		q += bytes;
	    }
	    event.tick = tick;
	    event.order = count;
	    if (count == capacity)
	    {
		capacity = capacity ? capacity * 2 : 1024;
		list = realloc (list, capacity * sizeof(*list));
		if (!list)
		    I_Error ("OPL music: out of memory");
	    }
	    list[count++] = event;
	}
    }
    if (!count)
    {
	free (list);
	return false;
    }
    qsort (list, count, sizeof(*list), compare_midi);
    {
	double seconds = 0, per_tick;
	uint32_t last = 0;

	/* SMPTE division: frames per second times ticks per frame. */
	if (division & 0x8000)
	    per_tick = 1.0 / ((256 - (division >> 8)) * (division & 0xff));
	else
	    per_tick = 0.5 / division;	/* 120 beats per minute */
	for (int i = 0; i < count; ++i)
	{
	    const midi_event_t* event = &list[i];
	    int channel = event->status & 15;
	    int type = event->status >> 4;

	    seconds += (event->tick - last) * per_tick;
	    last = event->tick;
	    if (event->status == 0xff)
	    {
		if (!(division & 0x8000))
		    per_tick = event->tempo / 1e6 / division;
		continue;
	    }
	    /* MIDI percussion is channel 10; in MUS numbering it is 15. */
	    channel = channel == 9 ? 15 : channel == 15 ? 9 : channel;
	    if (type == 0x8)
		add_event (seconds, EV_NOTE_OFF, channel, event->data1, 0);
	    else if (type == 0x9)
		add_event (seconds, EV_NOTE_ON, channel, event->data1, event->data2);
	    else if (type == 0xb)
		add_event (seconds, EV_CONTROLLER, channel, event->data1, event->data2);
	    else if (type == 0xc)
		add_event (seconds, EV_PROGRAM, channel, event->data1, 0);
	    else if (type == 0xe)
		add_event (seconds, EV_BEND, channel, event->data1, event->data2);
	}
	song_length = (uint32_t)(seconds * output_rate);
    }
    free (list);
    return true;
}

/*
 * Interface. The sound module holds the audio lock around these calls
 * and around OPL_RenderMusic.
 */
boolean OPL_InitMusic (int rate)
{
    if (ready || failed)
	return ready;
    output_rate = rate;
    if (M_CheckParm ("-nomusic") || !load_instruments ())
    {
	failed = true;
	return false;
    }
    OPL3_Reset (&chip, (uint32_t)rate);
    init_registers ();
    init_voices ();
    reset_song ();
    ready = true;
    printf ("OPL music ready (GENMIDI instruments, 18 voices).\n");
    return true;
}

void OPL_SetMusicVolume (int volume)
{
    music_gain = OUTPUT_GAIN * (volume < 0 ? 0 : volume >= 15 ? 1.0f : volume / 15.0f);
}

boolean OPL_RegisterSong (const void* data, int size)
{
    OPL_UnRegisterSong ();
    if (!ready || size < 4)
	return false;
    num_score = 0;
    if (!memcmp (data, "MThd", 4) ? !load_midi (data, size) : !load_mus (data, size))
    {
	fprintf (stderr, "Could not load the music score.\n");
	num_score = 0;
	return false;
    }
    song_loaded = true;
    return true;
}

void OPL_UnRegisterSong (void)
{
    OPL_StopSong ();
    song_loaded = false;
    num_score = 0;
}

void OPL_PlaySong (boolean loop)
{
    if (!song_loaded)
	return;
    reset_song ();
    looping = loop;
    playing = true;
    paused = false;
}

void OPL_StopSong (void)
{
    if (!ready)
	return;
    playing = false;
    reset_song ();
}

void OPL_PauseSong (void) { paused = true; }
void OPL_ResumeSong (void) { paused = false; }

void OPL_RenderMusic (float* stereo, int frames)
{
    int16_t sample[2];

    if (!ready || !playing || paused)
	return;
    for (int i = 0; i < frames; ++i)
    {
	while (next_event < num_score && score[next_event].frame <= song_frame)
	    process (&score[next_event++]);
	if (next_event == num_score && song_frame >= song_length)
	{
	    if (!looping)
	    {
		playing = false;
		reset_song ();
		return;
	    }
	    reset_song ();
	    continue;
	}
	OPL3_GenerateResampled (&chip, sample);
	stereo[2 * i] += sample[0] / 32768.0f * music_gain;
	stereo[2 * i + 1] += sample[1] / 32768.0f * music_gain;
	++song_frame;
    }
}
