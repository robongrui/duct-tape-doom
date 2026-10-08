/* Play standard MIDI and DOOM MUS scores through the Mac's GM synthesizer. */
#include <math.h>
#include <string.h>
#include "doomdef.h"
#include "i_sound.h"
#include "m_argv.h"
#include "w_wad.h"
#include "i_opl_music.h"
#include <AudioToolbox/AudioToolbox.h>

static MusicSequence sequence;
static MusicPlayer player;
static MusicTimeStamp length;
static AudioUnit volume_unit;
static int music_volume = 8;
static unsigned read16(const byte *p) { return p[0] | (unsigned)p[1] << 8; }

static int load_mus(const byte *data, int size)
{
    if (size < 16 || memcmp(data, "MUS\x1a", 4)) return 0;
    unsigned start = read16(data + 6), score_size = read16(data + 4);
    if (start > (unsigned)size || score_size > (unsigned)size - start) return 0;
    const byte *p = data + start, *end = p + score_size;
    MusicTrack track;
    if (MusicSequenceNewTrack(sequence, &track)) return 0;
    double note_time[16][128];
    byte velocity[16], note_velocity[16][128];
    for (int c = 0; c < 16; ++c)
    {
        velocity[c] = 127;
        for (int n = 0; n < 128; ++n) note_time[c][n] = -1;
    }
    double time = 0;
    const byte controllers[] = {0,32,1,7,10,11,91,93,64,67,120,123,126,127,121};
    while (p < end)
    {
        byte descriptor = *p++;
        int channel = descriptor & 15;
        int midi_channel = channel == 15 ? 9 : channel >= 9 ? channel + 1 : channel;
        int type = (descriptor >> 4) & 7;
        if (type == 6) break;
        if (p == end) return 0;
        byte value = *p++;
        if (type == 0 || type == 1)
        {
            int note = value & 127;
            if (note_time[channel][note] >= 0)
            {
                MIDINoteMessage event = {(UInt8)midi_channel, (UInt8)note,
                    note_velocity[channel][note], 0, (Float32)(time - note_time[channel][note])};
                MusicTrackNewMIDINoteEvent(track, note_time[channel][note], &event);
                note_time[channel][note] = -1;
            }
            if (type == 1)
            {
                if (value & 128) { if (p == end) return 0; velocity[channel] = *p++ & 127; }
                note_time[channel][note] = time;
                note_velocity[channel][note] = velocity[channel];
            }
        }
        else
        {
            MIDIChannelMessage event = {0,0,0,0};
            if (type == 2)
            {
                unsigned bend = (unsigned)value * 64;
                event.status = (UInt8)(0xe0 | midi_channel);
                event.data1 = bend & 127; event.data2 = bend >> 7;
            }
            else if (type == 3 && value >= 10 && value <= 14)
            {
                event.status = (UInt8)(0xb0 | midi_channel); event.data1 = controllers[value];
            }
            else if (type == 4 && value < 10)
            {
                if (p == end) return 0;
                byte setting = *p++;
                event.status = (UInt8)((value == 0 ? 0xc0 : 0xb0) | midi_channel);
                event.data1 = value == 0 ? setting & 127 : controllers[value];
                event.data2 = value == 0 ? 0 : setting > 127 ? 127 : setting;
            }
            else return 0;
            MusicTrackNewMIDIChannelEvent(track, time, &event);
        }
        if (descriptor & 128)
        {
            uint32_t delay = 0; byte next;
            do
            {
                if (p == end || delay > UINT32_MAX >> 7) return 0;
                next = *p++; delay = (delay << 7) | (next & 127);
            } while (next & 128);
            time += delay / 70.0; /* Default 120 BPM: 70 MUS ticks per beat. */
        }
    }
    for (int c = 0; c < 16; ++c)
        for (int n = 0; n < 128; ++n)
            if (note_time[c][n] >= 0)
            {
                int midi = c == 15 ? 9 : c >= 9 ? c + 1 : c;
                MIDINoteMessage event = {(UInt8)midi, (UInt8)n, note_velocity[c][n], 0,
                    (Float32)(time - note_time[c][n])};
                MusicTrackNewMIDINoteEvent(track, note_time[c][n], &event);
            }
    MusicTrackSetProperty(track, kSequenceTrackProperty_TrackLength, &time, sizeof(time));
    return 1;
}

/* -opl plays the scores through the OPL synthesizer instead. */
static int use_opl = -1;
static int opl(void)
{
    if (use_opl < 0) use_opl = M_CheckParm("-opl") != 0;
    return use_opl;
}

void I_InitMusic(void) { }
void I_SetMusicVolume(int volume)
{
    music_volume = volume;
    if (opl()) { I_OPL_SetMusicVolume(volume); return; }
    if (sequence)
    {
        UInt32 count = 0; MusicSequenceGetTrackCount(sequence, &count);
        for (UInt32 i = 0; i < count; ++i)
        {
            MusicTrack track; MusicSequenceGetIndTrack(sequence, i, &track);
            Boolean muted = volume <= 0;
            MusicTrackSetProperty(track, kSequenceTrackProperty_MuteStatus, &muted, sizeof(muted));
        }
    }
    if (volume_unit)
    {
        float gain = volume <= 0 ? -40.0f : 20.0f * log10f(volume / 15.0f);
        AudioUnitSetParameter(volume_unit, kDynamicsProcessorParam_OverallGain,
                              kAudioUnitScope_Global, 0, gain, 0);
    }
}
void I_UnRegisterSong(int handle)
{
    (void)handle;
    if (opl()) { I_OPL_ShutdownMusic(); return; }
    if (player) { MusicPlayerStop(player); DisposeMusicPlayer(player); player = NULL; }
    if (sequence) { DisposeMusicSequence(sequence); sequence = NULL; }
    volume_unit = NULL;
}
void I_ShutdownMusic(void) { I_UnRegisterSong(0); }

int I_RegisterSong(void *data)
{
    if (opl())
    {
        int registered = I_OPL_RegisterSong(data);
        I_OPL_SetMusicVolume(music_volume);
        return registered;
    }
    I_UnRegisterSong(0);
    if (M_CheckParm("-nosound") || M_CheckParm("-nomusic")) return 0;
    int size = 0;
    for (int i = 0; i < numlumps; ++i)
        if (lumpcache[i] == data) { size = W_LumpLength(i); break; }
    if (size < 4 || NewMusicSequence(&sequence)) return 0;
    int loaded = 0;
    if (!memcmp(data, "MThd", 4))
    {
        CFDataRef midi = CFDataCreate(NULL, data, size);
        if (midi) { loaded = !MusicSequenceFileLoadData(sequence, midi, kMusicSequenceFile_MIDIType, 0); CFRelease(midi); }
    }
    else loaded = load_mus(data, size);
    if (!loaded || NewMusicPlayer(&player) || MusicPlayerSetSequence(player, sequence))
    { I_UnRegisterSong(0); fprintf(stderr, "Could not load the music score.\n"); return 0; }
    UInt32 count = 0;
    MusicSequenceGetTrackCount(sequence, &count);
    length = 0;
    for (UInt32 i = 0; i < count; ++i)
    {
        MusicTrack track; MusicTimeStamp duration = 0; UInt32 size = sizeof(duration);
        MusicSequenceGetIndTrack(sequence, i, &track);
        MusicTrackGetProperty(track, kSequenceTrackProperty_TrackLength, &duration, &size);
        if (duration > length) length = duration;
    }
    AUGraph graph;
    if (!MusicSequenceGetAUGraph(sequence, &graph))
    {
        UInt32 nodes = 0; AUGraphGetNodeCount(graph, &nodes);
        for (UInt32 i = 0; i < nodes; ++i)
        {
            AUNode node; AudioComponentDescription description; AudioUnit unit;
            AUGraphGetIndNode(graph, i, &node);
            if (!AUGraphNodeInfo(graph, node, &description, &unit)
                && description.componentSubType == kAudioUnitSubType_DynamicsProcessor)
                volume_unit = unit;
        }
    }
    I_SetMusicVolume(music_volume);
    fprintf(stderr, "Mac music synthesizer ready.\n");
    return 1;
}
void I_PlaySong(int handle, int looping)
{
    if (opl()) { if (handle) I_OPL_PlaySong(looping); return; }
    if (!handle || !player) return;
    UInt32 count = 0; MusicSequenceGetTrackCount(sequence, &count);
    for (UInt32 i = 0; i < count; ++i)
    {
        MusicTrack track; MusicSequenceGetIndTrack(sequence, i, &track);
        MusicTrackSetProperty(track, kSequenceTrackProperty_TrackLength, &length, sizeof(length));
        MusicTrackLoopInfo loop = {looping ? length : 0, 0};
        MusicTrackSetProperty(track, kSequenceTrackProperty_LoopInfo, &loop, sizeof(loop));
    }
    MusicPlayerSetTime(player, 0);
    OSStatus error = MusicPlayerPreroll(player);
    if (!error) error = MusicPlayerStart(player);
    if (error) fprintf(stderr, "Music output unavailable (%d).\n", (int)error);
}
void I_StopSong(int handle)
{
    (void)handle;
    if (opl()) I_OPL_StopSong();
    else if (player) MusicPlayerStop(player);
}
void I_PauseSong(int handle)
{
    if (opl()) I_OPL_PauseSong();
    else I_StopSong(handle);
}
void I_ResumeSong(int handle)
{
    (void)handle;
    if (opl()) I_OPL_ResumeSong();
    else if (player) MusicPlayerStart(player);
}
