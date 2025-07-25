// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//-----------------------------------------------------------------------------
/// \file
/// \brief SDL interface for sound

#include <math.h>
#include "../doomtype.h"

#ifdef _MSC_VER
#pragma warning(disable : 4214 4244)
#endif

#ifdef SDL

#include "SDL.h"

#ifdef _MSC_VER
#pragma warning(default : 4214 4244)
#endif

#ifdef HAVE_MIXER
#include "SDL_mixer.h"
/* This is the version number macro for the current SDL_mixer version: */
#ifndef SDL_MIXER_COMPILEDVERSION
#define SDL_MIXER_COMPILEDVERSION \
	SDL_VERSIONNUM(MIX_MAJOR_VERSION, MIX_MINOR_VERSION, MIX_PATCHLEVEL)
#endif

/* This macro will evaluate to true if compiled with SDL_mixer at least X.Y.Z */
#ifndef SDL_MIXER_VERSION_ATLEAST
#define SDL_MIXER_VERSION_ATLEAST(X, Y, Z) \
	(SDL_MIXER_COMPILEDVERSION >= SDL_VERSIONNUM(X, Y, Z))
#endif

#else
#define MIX_CHANNELS 8
#endif

#if defined (_WIN32) && !defined (_WIN32_WCE) && !defined (_XBOX)
#include <direct.h>
#elif defined (__GNUC__)
#include <unistd.h>
#endif
#include "../z_zone.h"

#include "../m_swap.h"
#include "../i_system.h"
#include "../i_sound.h"
#include "../m_argv.h"
#include "../m_misc.h"
#include "../w_wad.h"
#include "../screen.h" //vid.WndParent
#include "../doomdef.h"
#include "../doomstat.h"
#include "../s_sound.h"

#include "../d_main.h"

#ifdef HW3SOUND
#include "../hardware/hw3dsdrv.h"
#include "../hardware/hw3sound.h"
#include "hwsym_sdl.h"
#endif

#if (defined (_WIN32) && !defined (_XBOX) && !defined(NOFMOD)) || defined (HAVE_FMOD)
#define FMODSOUND
#endif

#ifdef FMODSOUND
#ifdef __MINGW32__
#include <FMOD/fmod.h>
#include "../../tools/fmoddyn.h"
#include <FMOD/fmod_errors.h>
#else
#if 0
#include <fmod.h>
#include "../../tools/fmoddyn.h"
#include <fmod_errors.h>
#endif
#endif
#define FMODMEMORY
#endif

// The number of internal mixing channels,
//  the samples calculated for each mixing step,
//  the size of the 16bit, 2 hardware channel (stereo)
//  mixing buffer, and the samplerate of the raw data.

// Needed for calling the actual sound output.
#if defined (_WIN32_WCE) || defined (DC) || defined (PSP) || defined(GP2X)
#define NUM_CHANNELS            MIX_CHANNELS
#else
#define NUM_CHANNELS            MIX_CHANNELS*4
#endif

#define INDEXOFSFX(x) ((sfxinfo_t *)x - S_sfx)

#if defined (_WIN32_WCE) || defined (DC) || defined (PSP)
static Uint16 samplecount = 512; //Alam: .5KB samplecount at 11025hz is 46.439909297052154195011337868481ms of buffer
#elif defined(GP2X)
static Uint16 samplecount = 128;
#else
static Uint16 samplecount = 1024; //Alam: 1KB samplecount at 22050hz is 46.439909297052154195011337868481ms of buffer
#endif

typedef struct chan_struct
{
	// The channel data pointers, start and end.
	Uint8 *data; //static unsigned char *channels[NUM_CHANNELS];
	Uint8 *end; //static unsigned char *channelsend[NUM_CHANNELS];

	// pitch
	Uint32 realstep; // The channel step amount...
	Uint32 step;          //static UINT32 channelstep[NUM_CHANNELS];
	Uint32 stepremainder; //static UINT32 channelstepremainder[NUM_CHANNELS];
	Uint32 samplerate; // ... and a 0.16 bit remainder of last step.

	// Time/gametic that the channel started playing,
	//  used to determine oldest, which automatically
	//  has lowest priority.
	tic_t starttic; //static INT32 channelstart[NUM_CHANNELS];

	// The sound handle, determined on registration,
	//  used to unregister/stop/modify,
	INT32 handle; //static INT32 channelhandles[NUM_CHANNELS];

	// SFX id of the playing sound effect.
	void *id; // Used to catch duplicates (like chainsaw).
	sfxenum_t sfxid; //static INT32 channelids[NUM_CHANNELS];
	INT32 vol; //the channel volume
	INT32 sep; //the channel pan

	// Hardware left and right channel volume lookup.
	Sint16* leftvol_lookup; //static INT32 *channelleftvol_lookup[NUM_CHANNELS];
	Sint16* rightvol_lookup; //static INT32 *channelrightvol_lookup[NUM_CHANNELS];
} chan_t;

static chan_t channels[NUM_CHANNELS];

// Pitch to stepping lookup
static INT32 steptable[256];

// Volume lookups.
static Sint16 vol_lookup[128 * 256];

UINT8 sound_started = false;
static SDL_mutex *Snd_Mutex = NULL;

//SDL's Audio
static SDL_AudioSpec audio;

static SDL_bool musicStarted = SDL_FALSE;
#ifdef HAVE_MIXER
static SDL_mutex *Msc_Mutex = NULL;
/* FIXME: Make this file instance-specific */
#ifdef _arch_dreamcast
#define MIDI_PATH     "/ram"
#elif defined(GP2X)
#define MIDI_PATH     "/mnt/sd/srb2"
#define MIDI_PATH2    "/tmp/mnt/sd/srb2"
#else
#define MIDI_PATH     srb2home
#if defined (__unix__) || defined(__APPLE__) || defined (UNIXCOMMON)
#define MIDI_PATH2    "/tmp"
#endif
#endif
#define MIDI_TMPFILE  "srb2music"
#define MIDI_TMPFILE2 "srb2wav"
static INT32 musicvol = 62;

#if SDL_MIXER_VERSION_ATLEAST(1,2,2)
#define MIXER_POS //Mix_FadeInMusicPos in 1.2.2+
static void SDLCALL I_FinishMusic(void);
static double loopstartDig = 0.0l;
static SDL_bool loopingDig = SDL_FALSE;
static SDL_bool canlooping = SDL_TRUE;
#endif

#if SDL_MIXER_VERSION_ATLEAST(1,2,7)
#define USE_RWOPS // ok, USE_RWOPS is in here
#if defined (DC) || defined (_WIN32_WCE) || defined (_XBOX) //|| defined(_WIN32) || defined(GP2X)
#undef USE_RWOPS
#endif
#endif

#if SDL_MIXER_VERSION_ATLEAST(1,2,10)
//#define MIXER_INIT
#endif

#ifdef USE_RWOPS
static void * Smidi[2] = { NULL, NULL };
static SDL_bool canuseRW = SDL_TRUE;
#endif
static const char *fmidi[2] = { MIDI_TMPFILE, MIDI_TMPFILE2};

static const INT32 MIDIfade = 500;
static const INT32 Digfade = 0;

static Mix_Music *music[2] = { NULL, NULL };
#endif

#ifdef FMODSOUND
static FMOD_INSTANCE *fmod375 = NULL;
static FMUSIC_MODULE *mod = NULL;
static INT32 fsoundchannel = -1;
static INT32 fsoundfreq = 0;
static INT32 fmodvol = 127;
static FSOUND_STREAM *fmus = NULL;
#endif
static SDL_bool nofmodmusic = SDL_TRUE;

static inline void Snd_LockAudio(void) //Alam: Lock audio data and uninstall audio callback
{
	if (Snd_Mutex) SDL_LockMutex(Snd_Mutex);
	else if (nosound) return;
	else if (nomidimusic && nodigimusic
#ifdef HW3SOUND
	         && hws_mode == HWS_DEFAULT_MODE
#endif
	        ) SDL_LockAudio();
#ifdef HAVE_MIXER
	else if (musicStarted) Mix_SetPostMix(NULL, NULL);
#endif
}

static inline void Snd_UnlockAudio(void) //Alam: Unlock audio data and reinstall audio callback
{
	if (Snd_Mutex) SDL_UnlockMutex(Snd_Mutex);
	else if (nosound) return;
	else if (nomidimusic && nodigimusic
#ifdef HW3SOUND
	         && hws_mode == HWS_DEFAULT_MODE
#endif
	        ) SDL_UnlockAudio();
#ifdef HAVE_MIXER
	else if (musicStarted) Mix_SetPostMix(audio.callback, audio.userdata);
#endif
}

FUNCMATH static inline SDL_bool Snd_Convert(Uint16 sr)
{
#if 1
	return SDL_FALSE;
#endif
	return (sr > audio.freq) || (sr % 11025); // more samples then needed or odd samplerate
}

#ifdef _MSC_VER
#pragma warning(disable :  4200)
#pragma pack(1)
#endif

typedef struct
{
	Uint16 header;     // 3?
	Uint16 samplerate; // 11025+
	Uint16 samples;    // number of samples
	Uint16 dummy;     // 0
	Uint8  data[0];    // data;
} ATTRPACK dssfx_t;

#ifdef _MSC_VER
#pragma pack()
#pragma warning(default : 4200)
#endif

//
// This function loads the sound data from the WAD lump,
//  for single sound.
//
static void *getsfx(lumpnum_t sfxlump, size_t *len)
{
	dssfx_t *sfx, *paddedsfx;
	Uint16 sr;
	size_t size = *len;
	SDL_AudioCVT sfxcvt;

	sfx = (dssfx_t *)malloc(size);
	if (sfx) W_ReadLump(sfxlump, (void *)sfx);
	else return NULL;
	sr = SHORT(sfx->samplerate);

	if (Snd_Convert(sr) && SDL_BuildAudioCVT(&sfxcvt, AUDIO_U8, 1, sr, AUDIO_U8, 1, audio.freq))
	{//Alam: Setup the AudioCVT for the SFX

		sfxcvt.len = (INT32)size-8; //Alam: Chop off the header
		sfxcvt.buf = malloc(sfxcvt.len * sfxcvt.len_mult); //Alam: make room
		if (sfxcvt.buf) M_Memcpy(sfxcvt.buf, &(sfx->data), sfxcvt.len); //Alam: copy the sfx sample

		if (sfxcvt.buf && SDL_ConvertAudio(&sfxcvt) == 0) //Alam: let convert it!
		{
				size = sfxcvt.len_cvt + 8;
				*len = sfxcvt.len_cvt;

				// Allocate from zone memory.
				paddedsfx = (dssfx_t *) Z_Malloc(size, PU_SOUND, NULL);

				// Now copy and pad.
				M_Memcpy(paddedsfx+8, sfxcvt.buf, sfxcvt.len_cvt);
				free(sfxcvt.buf);
				M_Memcpy(paddedsfx,sfx,8);
				paddedsfx->samplerate = SHORT((Uint16)audio.freq); // new freq
		}
		else //Alam: the convert failed, not needed or i couldn't malloc the buf
		{
			if (sfxcvt.buf) free(sfxcvt.buf);
			*len = size - 8;

			// Allocate from zone memory then copy and pad
			paddedsfx = (dssfx_t *)M_Memcpy(Z_Malloc(size, PU_SOUND, NULL), sfx, size);
		}
	}
	else
	{
		// Pads the sound effect out to the mixing buffer size.
		// The original realloc would interfere with zone memory.
		*len = size - 8;

		// Allocate from zone memory then copy and pad
		paddedsfx = (dssfx_t *)M_Memcpy(Z_Malloc(size, PU_SOUND, NULL), sfx, size);
	}

	// Remove the cached lump.
	free(sfx);

	// Return allocated padded data.
	return paddedsfx;
}

// used to (re)calculate channel params based on vol, sep, pitch
static void I_SetChannelParams(chan_t *c, INT32 vol, INT32 sep, INT32 step)
{
	INT32 leftvol;
	INT32 rightvol;
	c->vol = vol;
	c->sep = sep;
	c->step = c->realstep = step;

	if (step != steptable[128])
		c->step += (((c->samplerate<<16)/audio.freq)-65536);
	else if (c->samplerate != (unsigned)audio.freq)
		c->step = ((c->samplerate<<16)/audio.freq);
	// x^2 separation, that is, orientation/stereo.
	//  range is: 0 (left) - 255 (right)

	// Volume arrives in range 0..255 and it must be in 0..cv_soundvolume...
	vol = (vol * cv_soundvolume.value) >> 7;
	// note: >> 6 would use almost the entire dynamical range, but
	// then there would be no "dynamical room" for other sounds :-/

	leftvol  = vol - ((vol*sep*sep) >> 16); ///(256*256);
	sep = 255 - sep;
	rightvol = vol - ((vol*sep*sep) >> 16);

	// Sanity check, clamp volume.
	if (rightvol < 0 || rightvol > 127)
	{
		rightvol = 63;
		//I_Error("rightvol out of bounds");
	}

	if (leftvol < 0 || leftvol > 127)
	{
		leftvol = 63;
		//I_Error("leftvol out of bounds");
	}

	// Get the proper lookup table piece
	//  for this volume level
	c->leftvol_lookup = &vol_lookup[leftvol*256];
	c->rightvol_lookup = &vol_lookup[rightvol*256];
}

static INT32 FindChannel(INT32 handle)
{
	INT32 i;

	for (i = 0; i < NUM_CHANNELS; i++)
		if (channels[i].handle == handle)
			return i;

	// not found
	return -1;
}

//
// This function adds a sound to the
//  list of currently active sounds,
//  which is maintained as a given number
//  (eight, usually) of internal channels.
// Returns a handle.
//
static INT32 addsfx(sfxenum_t sfxid, INT32 volume, INT32 step, INT32 seperation)
{
	static UINT16 handlenums = 0;
	INT32 i, slot, oldestnum = 0;
	tic_t oldest = gametic;

	// Play these sound effects only one at a time.
#if 1
	if (
#if 0
	sfxid == sfx_stnmov || sfxid == sfx_sawup || sfxid == sfx_sawidl || sfxid == sfx_sawful || sfxid == sfx_sawhit || sfxid == sfx_pistol
#else
	    ( sfx_litng1 <= sfxid && sfxid >= sfx_litng4 )
	    || sfxid == sfx_trfire || sfxid == sfx_alarm || sfxid == sfx_spin
	    || sfxid == sfx_athun1 || sfxid == sfx_athun2 || sfxid == sfx_rainin
#endif
	   )
	{
		// Loop all channels, check.
		for (i = 0; i < NUM_CHANNELS; i++)
		{
			// Active, and using the same SFX?
			if ((channels[i].end) && (channels[i].sfxid == sfxid))
			{
				// Reset.
				channels[i].end = NULL;
				// We are sure that iff,
				//  there will only be one.
				break;
			}
		}
	}
#endif

	// Loop all channels to find oldest SFX.
	for (i = 0; (i < NUM_CHANNELS) && (channels[i].end); i++)
	{
		if (channels[i].starttic < oldest)
		{
			oldestnum = i;
			oldest = channels[i].starttic;
		}
	}

	// Tales from the cryptic.
	// If we found a channel, fine.
	// If not, we simply overwrite the first one, 0.
	// Probably only happens at startup.
	if (i == NUM_CHANNELS)
		slot = oldestnum;
	else
		slot = i;

	channels[slot].end = NULL;
	// Okay, in the less recent channel,
	//  we will handle the new SFX.
	// Set pointer to raw data.
	channels[slot].data = (Uint8 *)S_sfx[sfxid].data;
	channels[slot].samplerate = (channels[slot].data[3]<<8)+channels[slot].data[2];
	channels[slot].data += 8; //Alam: offset of the sound header

	while (FindChannel(handlenums)!=-1)
	{
		handlenums++;
		// Reset current handle number, limited to 0..65535.
		if (handlenums == UINT16_MAX)
			handlenums = 0;
	}

	// Assign current handle number.
	// Preserved so sounds could be stopped.
	channels[slot].handle = handlenums;

	// Restart steper
	channels[slot].stepremainder = 0;
	// Should be gametic, I presume.
	channels[slot].starttic = gametic;

	I_SetChannelParams(&channels[slot], volume, seperation, step);

	// Preserve sound SFX id,
	//  e.g. for avoiding duplicates of chainsaw.
	channels[slot].id = S_sfx[sfxid].data;

	channels[slot].sfxid = sfxid;

	// Set pointer to end of raw data.
	channels[slot].end = channels[slot].data + S_sfx[sfxid].length;


	// You tell me.
	return handlenums;
}

//
// SFX API
// Note: this was called by S_Init.
// However, whatever they did in the
// old DPMS based DOS version, this
// were simply dummies in the Linux
// version.
// See soundserver initdata().
//
// Well... To keep compatibility with legacy doom, I have to call this in
// I_InitSound since it is not called in S_Init... (emanne@absysteme.fr)

static inline void I_SetChannels(void)
{
	// Init internal lookups (raw data, mixing buffer, channels).
	// This function sets up internal lookups used during
	//  the mixing process.
	INT32 i;
	INT32 j;

	INT32 *steptablemid = steptable + 128;

	if (nosound)
		return;

	// This table provides step widths for pitch parameters.
	for (i = -128; i < 128; i++)
	{
		const double po = pow((double)(2.0l), (double)(i / 64.0l));
		steptablemid[i] = (INT32)(po * 65536.0l);
	}

	// Generates volume lookup tables
	//  which also turn the unsigned samples
	//  into signed samples.
	for (i = 0; i < 128; i++)
		for (j = 0; j < 256; j++)
		{
			//From PrDoom
			// proff - made this a little bit softer, because with
			// full volume the sound clipped badly
			vol_lookup[i * 256 + j] = (Sint16)((i * (j - 128) * 256) / 191);
			//Alam: hmm, lighting = !@#?
			//vol_lookup[i * 256 + j] = (Sint16)((i * (j - 128) * 256) / 127);
		}
}

void I_SetSfxVolume(INT32 volume)
{
	INT32 i;

	(void)volume;
	//Snd_LockAudio();

	for (i = 0; i < NUM_CHANNELS; i++)
		if (channels[i].end) I_SetChannelParams(&channels[i], channels[i].vol, channels[i].sep, channels[i].realstep);

	//Snd_UnlockAudio();
}

void *I_GetSfx(sfxinfo_t *sfx)
{
	if (sfx->lumpnum == LUMPERROR)
		sfx->lumpnum = S_GetSfxLumpNum(sfx);
//	else if (sfx->lumpnum != S_GetSfxLumpNum(sfx))
//		I_FreeSfx(sfx);

#ifdef HW3SOUND
	if (hws_mode != HWS_DEFAULT_MODE)
		return HW3S_GetSfx(sfx);
#endif

	if (sfx->data)
		return sfx->data; //Alam: I have it done!

	sfx->length = W_LumpLength(sfx->lumpnum);

	return getsfx(sfx->lumpnum, &sfx->length);

}

void I_FreeSfx(sfxinfo_t * sfx)
{
//	if (sfx->lumpnum<0)
//		return;

#ifdef HW3SOUND
	if (hws_mode != HWS_DEFAULT_MODE)
	{
		HW3S_FreeSfx(sfx);
	}
	else
#endif
	{
		size_t i;

		for (i = 1; i < NUMSFX; i++)
		{
			// Alias? Example is the chaingun sound linked to pistol.
			if (S_sfx[i].data == sfx->data)
			{
				if (S_sfx+i != sfx) S_sfx[i].data = NULL;
				S_sfx[i].lumpnum = LUMPERROR;
				S_sfx[i].length = 0;
			}
		}
		//Snd_LockAudio(); //Alam: too much?
		// Loop all channels, check.
		for (i = 0; i < NUM_CHANNELS; i++)
		{
			// Active, and using the same SFX?
			if (channels[i].end && channels[i].id == sfx->data)
			{
				channels[i].end = NULL; // Reset.
			}
		}
		//Snd_UnlockAudio(); //Alam: too much?
		Z_Free(sfx->data);
	}
	sfx->data = NULL;
	sfx->lumpnum = LUMPERROR;
}

//
// Starting a sound means adding it
//  to the current list of active sounds
//  in the internal channels.
// As the SFX info struct contains
//  e.g. a pointer to the raw data,
//  it is ignored.
// As our sound handling does not handle
//  priority, it is ignored.
// Pitching (that is, increased speed of playback)
//  is set, but currently not used by mixing.
//
INT32 I_StartSound(sfxenum_t id, INT32 vol, INT32 sep, INT32 pitch, INT32 priority)
{
	(void)priority;
	(void)pitch;

	if (nosound)
		return 0;

	if (S_sfx[id].data == NULL) return -1;

	Snd_LockAudio();
	id = addsfx(id, vol, steptable[pitch], sep);
	Snd_UnlockAudio();

	return id; // Returns a handle (not used).
}

void I_StopSound(INT32 handle)
{
	// You need the handle returned by StartSound.
	// Would be looping all channels,
	//  tracking down the handle,
	//  an setting the channel to zero.
	INT32 i;

	i = FindChannel(handle);

	if (i != -1)
	{
		//Snd_LockAudio(); //Alam: too much?
		channels[i].end = NULL;
		//Snd_UnlockAudio(); //Alam: too much?
		channels[i].handle = -1;
		channels[i].starttic = 0;
	}

}

INT32 I_SoundIsPlaying(INT32 handle)
{
	INT32 isplaying = false, chan = FindChannel(handle);
	if (chan != -1)
		isplaying = (channels[chan].end != NULL);
	return isplaying;
}

FUNCINLINE static ATTRINLINE void I_UpdateStream8S(Uint8 *stream, int len)
{
	// Mix current sound data.
	// Data, from raw sound
	register Sint16 dr; // Right 8bit stream
	register Uint8 sample; // Center 8bit sfx
	register Sint16 dl; // Left 8bit stream

	// Pointers in audio stream
	Sint8 *rightout = (Sint8 *)stream; // currect right
	Sint8 *leftout = rightout + 1;// currect left
	const Uint8 step = 2; // Step in stream, left and right, thus two.

	INT32 chan; // Mixing channel index.

	// Determine end of the stream
	len /= 2; // not 8bit mono samples, 8bit stereo ones

	if (Snd_Mutex) SDL_LockMutex(Snd_Mutex);

	// Mix sounds into the mixing buffer.
	// Loop over len
	while (len--)
	{
		// Reset left/right value.
		dl = *leftout;
		dr = *rightout;

		// Love thy L2 cache - made this a loop.
		// Now more channels could be set at compile time
		//  as well. Thus loop those channels.
		for (chan = 0; chan < NUM_CHANNELS; chan++)
		{
			// Check channel, if active.
			if (channels[chan].end)
			{
#if 1
				// Get the raw data from the channel.
				sample = channels[chan].data[0];
#else
				// linear filtering from PrDoom
				sample = (((Uint32)channels[chan].data[0] *(0x10000 - channels[chan].stepremainder))
					+ ((Uint32)channels[chan].data[1]) * (channels[chan].stepremainder))) >> 16;
#endif
				// Add left and right part
				//  for this channel (sound)
				//  to the current data.
				// Adjust volume accordingly.
				dl = (Sint16)(dl+(channels[chan].leftvol_lookup[sample]>>8));
				dr = (Sint16)(dr+(channels[chan].rightvol_lookup[sample]>>8));
				// Increment stepage
				channels[chan].stepremainder += channels[chan].step;
				// Check whether we are done.
				if (channels[chan].data + (channels[chan].stepremainder >> 16) >= channels[chan].end)
					channels[chan].end = NULL;
				else
				{
					// step to next sample
					channels[chan].data += (channels[chan].stepremainder >> 16);
					// Limit to LSB???
					channels[chan].stepremainder &= 0xffff;
				}
			}
		}

		// Clamp to range. Left hardware channel.
		// Has been char instead of short.

		if (dl > 0x7f)
			*leftout = 0x7f;
		else if (dl < -0x80)
			*leftout = -0x80;
		else
			*leftout = (Sint8)dl;

		// Same for right hardware channel.
		if (dr > 0x7f)
			*rightout = 0x7f;
		else if (dr < -0x80)
			*rightout = -0x80;
		else
			*rightout = (Sint8)dr;

		// Increment current pointers in stream
		leftout += step;
		rightout += step;

	}
	if (Snd_Mutex) SDL_UnlockMutex(Snd_Mutex);
}

FUNCINLINE static ATTRINLINE void I_UpdateStream8M(Uint8 *stream, int len)
{
	// Mix current sound data.
	// Data, from raw sound
	register Sint16 d; // Mono 8bit stream
	register Uint8 sample; // Center 8bit sfx

	// Pointers in audio stream
	Sint8 *monoout = (Sint8 *)stream; // currect mono
	const Uint8 step = 1; // Step in stream, left and right, thus two.

	INT32 chan; // Mixing channel index.

	// Determine end of the stream
	//len /= 1; // not 8bit mono samples, 8bit mono ones?

	if (Snd_Mutex) SDL_LockMutex(Snd_Mutex);

	// Mix sounds into the mixing buffer.
	// Loop over len
	while (len--)
	{
		// Reset left/right value.
		d = *monoout;

		// Love thy L2 cache - made this a loop.
		// Now more channels could be set at compile time
		//  as well. Thus loop those channels.
		for (chan = 0; chan < NUM_CHANNELS; chan++)
		{
			// Check channel, if active.
			if (channels[chan].end)
			{
#if 1
				// Get the raw data from the channel.
				sample = channels[chan].data[0];
#else
				// linear filtering from PrDoom
				sample = (((Uint32)channels[chan].data[0] *(0x10000 - channels[chan].stepremainder))
					+ ((Uint32)channels[chan].data[1]) * (channels[chan].stepremainder))) >> 16;
#endif
				// Add left and right part
				//  for this channel (sound)
				//  to the current data.
				// Adjust volume accordingly.
				d = (Sint16)(d+((channels[chan].leftvol_lookup[sample] + channels[chan].rightvol_lookup[sample])>>9));
				// Increment stepage
				channels[chan].stepremainder += channels[chan].step;
				// Check whether we are done.
				if (channels[chan].data + (channels[chan].stepremainder >> 16) >= channels[chan].end)
					channels[chan].end = NULL;
				else
				{
					// step to next sample
					channels[chan].data += (channels[chan].stepremainder >> 16);
					// Limit to LSB???
					channels[chan].stepremainder &= 0xffff;
				}
			}
		}

		// Clamp to range. Left hardware channel.
		// Has been char instead of short.

		if (d > 0x7f)
			*monoout = 0x7f;
		else if (d < -0x80)
			*monoout = -0x80;
		else
			*monoout = (Sint8)d;

		// Increment current pointers in stream
		monoout += step;
	}
	if (Snd_Mutex) SDL_UnlockMutex(Snd_Mutex);
}

FUNCINLINE static ATTRINLINE void I_UpdateStream16S(Uint8 *stream, int len)
{
	// Mix current sound data.
	// Data, from raw sound
	register Sint32 dr; // Right 16bit stream
	register Uint8 sample; // Center 8bit sfx
	register Sint32 dl; // Left 16bit stream

	// Pointers in audio stream
	Sint16 *rightout = (Sint16 *)(void *)stream; // currect right
	Sint16 *leftout = rightout + 1;// currect left
	const Uint8 step = 2; // Step in stream, left and right, thus two.

	INT32 chan; // Mixing channel index.

	// Determine end of the stream
	len /= 4; // not 8bit mono samples, 16bit stereo ones

	if (Snd_Mutex) SDL_LockMutex(Snd_Mutex);

	// Mix sounds into the mixing buffer.
	// Loop over len
	while (len--)
	{
		// Reset left/right value.
		dl = *leftout;
		dr = *rightout;

		// Love thy L2 cache - made this a loop.
		// Now more channels could be set at compile time
		//  as well. Thus loop those channels.
		for (chan = 0; chan < NUM_CHANNELS; chan++)
		{
			// Check channel, if active.
			if (channels[chan].end)
			{
#if 1
				// Get the raw data from the channel.
				sample = channels[chan].data[0];
#else
				// linear filtering from PrDoom
				sample = (((Uint32)channels[chan].data[0] *(0x10000 - channels[chan].stepremainder))
					+ ((Uint32)channels[chan].data[1]) * (channels[chan].stepremainder))) >> 16;
#endif
				// Add left and right part
				//  for this channel (sound)
				//  to the current data.
				// Adjust volume accordingly.
				dl += channels[chan].leftvol_lookup[sample];
				dr += channels[chan].rightvol_lookup[sample];
				// Increment stepage
				channels[chan].stepremainder += channels[chan].step;
				// Check whether we are done.
				if (channels[chan].data + (channels[chan].stepremainder >> 16) >= channels[chan].end)
					channels[chan].end = NULL;
				else
				{
					// step to next sample
					channels[chan].data += (channels[chan].stepremainder >> 16);
					// Limit to LSB???
					channels[chan].stepremainder &= 0xffff;
				}
			}
		}

		// Clamp to range. Left hardware channel.
		// Has been char instead of short.

		if (dl > 0x7fff)
			*leftout = 0x7fff;
		else if (dl < -0x8000)
			*leftout = -0x8000;
		else
			*leftout = (Sint16)dl;

		// Same for right hardware channel.
		if (dr > 0x7fff)
			*rightout = 0x7fff;
		else if (dr < -0x8000)
			*rightout = -0x8000;
		else
			*rightout = (Sint16)dr;

		// Increment current pointers in stream
		leftout += step;
		rightout += step;

	}
	if (Snd_Mutex) SDL_UnlockMutex(Snd_Mutex);
}

FUNCINLINE static ATTRINLINE void I_UpdateStream16M(Uint8 *stream, int len)
{
	// Mix current sound data.
	// Data, from raw sound
	register Sint32 d; // Mono 16bit stream
	register Uint8 sample; // Center 8bit sfx

	// Pointers in audio stream
	Sint16 *monoout = (Sint16 *)(void *)stream; // currect mono
	const Uint8 step = 1; // Step in stream, left and right, thus two.

	INT32 chan; // Mixing channel index.

	// Determine end of the stream
	len /= 2; // not 8bit mono samples, 16bit mono ones

	if (Snd_Mutex) SDL_LockMutex(Snd_Mutex);

	// Mix sounds into the mixing buffer.
	// Loop over len
	while (len--)
	{
		// Reset left/right value.
		d = *monoout;

		// Love thy L2 cache - made this a loop.
		// Now more channels could be set at compile time
		//  as well. Thus loop those channels.
		for (chan = 0; chan < NUM_CHANNELS; chan++)
		{
			// Check channel, if active.
			if (channels[chan].end)
			{
#if 1
				// Get the raw data from the channel.
				sample = channels[chan].data[0];
#else
				// linear filtering from PrDoom
				sample = (((Uint32)channels[chan].data[0] *(0x10000 - channels[chan].stepremainder))
					+ ((Uint32)channels[chan].data[1]) * (channels[chan].stepremainder))) >> 16;
#endif
				// Add left and right part
				//  for this channel (sound)
				//  to the current data.
				// Adjust volume accordingly.
				d += (channels[chan].leftvol_lookup[sample] + channels[chan].rightvol_lookup[sample])>>1;
				// Increment stepage
				channels[chan].stepremainder += channels[chan].step;
				// Check whether we are done.
				if (channels[chan].data + (channels[chan].stepremainder >> 16) >= channels[chan].end)
					channels[chan].end = NULL;
				else
				{
					// step to next sample
					channels[chan].data += (channels[chan].stepremainder >> 16);
					// Limit to LSB???
					channels[chan].stepremainder &= 0xffff;
				}
			}
		}

		// Clamp to range. Left hardware channel.
		// Has been char instead of short.

		if (d > 0x7fff)
			*monoout = 0x7fff;
		else if (d < -0x8000)
			*monoout = -0x8000;
		else
			*monoout = (Sint16)d;

		// Increment current pointers in stream
		monoout += step;
	}
	if (Snd_Mutex) SDL_UnlockMutex(Snd_Mutex);
}

static void SDLCALL I_UpdateStream(void *userdata, Uint8 *stream, int len)
{
	if (!sound_started || !userdata)
		return;

#if SDL_VERSION_ATLEAST(1,3,0)
	if (musicStarted)
		memset(stream, 0x00, len); // only work in !AUDIO_U8, that needs 0x80
#endif

	if ((audio.channels != 1 && audio.channels != 2) ||
	    (audio.format != AUDIO_S8 && audio.format != AUDIO_S16SYS))
		; // no function to encode this type of stream
	else if (audio.channels == 1 && audio.format == AUDIO_S8)
		I_UpdateStream8M(stream, len);
	else if (audio.channels == 2 && audio.format == AUDIO_S8)
		I_UpdateStream8S(stream, len);
	else if (audio.channels == 1 && audio.format == AUDIO_S16SYS)
		I_UpdateStream16M(stream, len);
	else if (audio.channels == 2 && audio.format == AUDIO_S16SYS)
		I_UpdateStream16S(stream, len);
}

void I_UpdateSoundParams(INT32 handle, INT32 vol, INT32 sep, INT32 pitch)
{
	// Would be using the handle to identify
	//  on which channel the sound might be active,
	//  and resetting the channel parameters.

	INT32 i = FindChannel(handle);
	(void)pitch;

	if (i != -1 && channels[i].end)
	{
		//Snd_LockAudio(); //Alam: too much?
		I_SetChannelParams(&channels[i], vol, sep, steptable[pitch]);
		//Snd_UnlockAudio(); //Alam: too much?
	}

}

#ifdef HW3SOUND

static void *soundso = NULL;

static INT32 Init3DSDriver(const char *soName)
{
	if (soName) soundso = hwOpen(soName);
#if defined (_WIN32) && defined (_X86_) && !defined (STATIC3DS)
	HW3DS.pfnStartup            = hwSym("_Startup@8",soundso);
	HW3DS.pfnShutdown           = hwSym("_Shutdown@0",soundso);
	HW3DS.pfnAddSfx             = hwSym("_AddSfx@4",soundso);
	HW3DS.pfnAddSource          = hwSym("_AddSource@8",soundso);
	HW3DS.pfnStartSource        = hwSym("_StartSource@4",soundso);
	HW3DS.pfnStopSource         = hwSym("_StopSource@4",soundso);
	HW3DS.pfnGetHW3DSVersion    = hwSym("_GetHW3DSVersion@0",soundso);
	HW3DS.pfnBeginFrameUpdate   = hwSym("_BeginFrameUpdate@0",soundso);
	HW3DS.pfnEndFrameUpdate     = hwSym("_EndFrameUpdate@0",soundso);
	HW3DS.pfnIsPlaying          = hwSym("_IsPlaying@4",soundso);
	HW3DS.pfnUpdateListener     = hwSym("_UpdateListener@8",soundso);
	HW3DS.pfnUpdateSourceParms  = hwSym("_UpdateSourceParms@12",soundso);
	HW3DS.pfnSetCone            = hwSym("_SetCone@8",soundso);
	HW3DS.pfnSetGlobalSfxVolume = hwSym("_SetGlobalSfxVolume@4",soundso);
	HW3DS.pfnUpdate3DSource     = hwSym("_Update3DSource@8",soundso);
	HW3DS.pfnReloadSource       = hwSym("_ReloadSource@8",soundso);
	HW3DS.pfnKillSource         = hwSym("_KillSource@4",soundso);
	HW3DS.pfnKillSfx            = hwSym("_KillSfx@4",soundso);
	HW3DS.pfnGetHW3DSTitle      = hwSym("_GetHW3DSTitle@8",soundso);
#else
	HW3DS.pfnStartup            = hwSym("Startup",soundso);
	HW3DS.pfnShutdown           = hwSym("Shutdown",soundso);
	HW3DS.pfnAddSfx             = hwSym("AddSfx",soundso);
	HW3DS.pfnAddSource          = hwSym("AddSource",soundso);
	HW3DS.pfnStartSource        = hwSym("StartSource",soundso);
	HW3DS.pfnStopSource         = hwSym("StopSource",soundso);
	HW3DS.pfnGetHW3DSVersion    = hwSym("GetHW3DSVersion",soundso);
	HW3DS.pfnBeginFrameUpdate   = hwSym("BeginFrameUpdate",soundso);
	HW3DS.pfnEndFrameUpdate     = hwSym("EndFrameUpdate",soundso);
	HW3DS.pfnIsPlaying          = hwSym("IsPlaying",soundso);
	HW3DS.pfnUpdateListener     = hwSym("UpdateListener",soundso);
	HW3DS.pfnUpdateSourceParms  = hwSym("UpdateSourceParms",soundso);
	HW3DS.pfnSetCone            = hwSym("SetCone",soundso);
	HW3DS.pfnSetGlobalSfxVolume = hwSym("SetGlobalSfxVolume",soundso);
	HW3DS.pfnUpdate3DSource     = hwSym("Update3DSource",soundso);
	HW3DS.pfnReloadSource       = hwSym("ReloadSource",soundso);
	HW3DS.pfnKillSource         = hwSym("KillSource",soundso);
	HW3DS.pfnKillSfx            = hwSym("KillSfx",soundso);
	HW3DS.pfnGetHW3DSTitle      = hwSym("GetHW3DSTitle",soundso);
#endif

//	if (HW3DS.pfnUpdateListener2 && HW3DS.pfnUpdateListener2 != soundso)
		return true;
//	else
//		return false;
}
#endif

void I_ShutdownSound(void)
{
	if (nosound || !sound_started)
		return;

	CONS_Printf("I_ShutdownSound: ");

#ifdef HW3SOUND
	if (hws_mode != HWS_DEFAULT_MODE)
	{
		HW3S_Shutdown();
		hwClose(soundso);
		return;
	}
#endif

	if (nomidimusic && nodigimusic)
		SDL_CloseAudio();
	CONS_Printf("shut down\n");
	sound_started = false;
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
	if (Snd_Mutex)
		SDL_DestroyMutex(Snd_Mutex);
	Snd_Mutex = NULL;
}

void I_StartupSound(void)
{
#ifdef HW3SOUND
	const char *sdrv_name = NULL;
#endif
#ifndef HAVE_MIXER
	nomidimusic = nodigimusic = true;
#endif
#ifdef DC
	//nosound = true;
#ifdef HAVE_MIXER
	nomidimusic = true;
	nodigimusic = true;
#endif
#endif

	memset(channels, 0, sizeof (channels)); //Alam: Clean it

	audio.format = AUDIO_S16SYS;
	audio.channels = 2;
	audio.callback = I_UpdateStream;
	audio.userdata = channels;

	if (dedicated)
	{
		nosound = nomidimusic = nodigimusic = true;
		return;
	}

	// Configure sound device
	CONS_Printf("I_StartupSound:\n");

	// Open the audio device
	if (M_CheckParm ("-freq") && M_IsNextParm())
	{
		audio.freq = atoi(M_GetNextParm());
		if (!audio.freq) audio.freq = cv_samplerate.value;
		audio.samples = (Uint16)(samplecount*(INT32)(audio.freq/22050)); //Alam: to keep it around the same XX ms
		CONS_Printf (" requested frequency of %d hz\n", audio.freq);
	}
	else
	{
		audio.samples = samplecount;
		audio.freq = cv_samplerate.value;
	}

	if (M_CheckParm ("-mono"))
	{
		audio.channels = 1;
		audio.samples /= 2;
	}

#if defined (_PSP)  && defined (HAVE_MIXER) // Bug in PSP's SDL_OpenAudio, can not open twice
	I_SetChannels();
	sound_started = true;
	Snd_Mutex = SDL_CreateMutex();
#else
	if (nosound)
#endif
		return;

#ifdef HW3SOUND
#ifdef STATIC3DS
	if (M_CheckParm("-3dsound") || M_CheckParm("-ds3d"))
	{
		hws_mode = HWS_OPENAL;
	}
#elif defined (_WIN32)
	if (M_CheckParm("-ds3d"))
	{
		hws_mode = HWS_DS3D;
		sdrv_name = "s_ds3d.dll";
	}
	else if (M_CheckParm("-fmod3d"))
	{
		hws_mode = HWS_FMOD3D;
		sdrv_name = "s_fmod.dll";
	}
	else if (M_CheckParm("-openal"))
	{
		hws_mode = HWS_OPENAL;
		sdrv_name = "s_openal.dll";
	}
#else
	if (M_CheckParm("-fmod3d"))
	{
		hws_mode = HWS_FMOD3D;
		sdrv_name = "./s_fmod.so";
	}
	else if (M_CheckParm("-openal"))
	{
		hws_mode = HWS_OPENAL;
		sdrv_name = "./s_openal.so";
	}
#endif
	else if (M_CheckParm("-sounddriver") &&  M_IsNextParm())
	{
		hws_mode = HWS_OTHER;
		sdrv_name = M_GetNextParm();
	}
	if (hws_mode != HWS_DEFAULT_MODE)
	{
		if (Init3DSDriver(sdrv_name))
		{
			snddev_t            snddev;

			//nosound = true;
			//I_AddExitFunc(I_ShutdownSound);
			snddev.bps = 16;
			snddev.sample_rate = audio.freq;
			snddev.numsfxs = NUMSFX;
#if defined (_WIN32) && !defined (_XBOX)
			snddev.cooplevel = 0x00000002;
			snddev.hWnd = vid.WndParent;
#endif
			if (HW3S_Init(I_Error, &snddev))
			{
				audio.userdata = NULL;
				CONS_Printf(" Using 3D sound driver\n");
				return;
			}
			CONS_Printf(" Failed 3D sound Init\n");
			// Falls back to default sound system
			HW3S_Shutdown();
			hwClose(soundso);
		}
		CONS_Printf(" Failed loading 3D sound driver\n");
		hws_mode = HWS_DEFAULT_MODE;
	}
#endif
	if (!musicStarted && SDL_OpenAudio(&audio, &audio) < 0)
	{
		CONS_Printf(" couldn't open audio with desired format\n");
		nosound = true;
		return;
	}
	else
	{
		char ad[100];
		CONS_Printf(" Starting up with audio driver : %s\n", SDL_AudioDriverName(ad, (int)sizeof ad));
	}
	samplecount = audio.samples;
	CV_SetValue(&cv_samplerate, audio.freq);
	CONS_Printf(" configured audio device with %d samples/slice at %ikhz(%dms buffer)\n", samplecount, audio.freq/1000, (INT32) (((float)audio.samples * 1000.0f) / audio.freq));
	// Finished initialization.
	CONS_Printf("I_InitSound: sound module ready\n");
	//[segabor]
	if (!musicStarted) SDL_PauseAudio(0);
	//Mix_Pause(0);
	I_SetChannels();
	sound_started = true;
	Snd_Mutex = SDL_CreateMutex();
}

//
// MUSIC API.
//

void I_ShutdownMIDIMusic(void)
{
	nomidimusic = false;
	if (nodigimusic) I_ShutdownMusic();
}

#ifdef FMODSOUND
static void I_ShutdownFMODMusic(void)
{
	CONS_Printf("I_ShutdownFMODMusic:\n");
	if (fmod375 && fmod375->FSOUND_GetError() != FMOD_ERR_UNINITIALIZED)
	{
		if (fmod375->FSOUND_GetError() != FMOD_ERR_NONE && fmod375->FSOUND_GetError() != FMOD_ERR_CHANNEL_ALLOC && fmod375->FSOUND_GetError() != FMOD_ERR_MEDIAPLAYER)
			if (devparm) I_OutputMsg("FMOD(Shutdown,Unknown): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
		if (mod)
		{
			if (fmod375->FMUSIC_IsPlaying(mod))
				if (!fmod375->FMUSIC_StopSong(mod))
					if (devparm) I_OutputMsg("FMOD(Shutdown,FMUSIC_StopSong): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
			if (!fmod375->FMUSIC_FreeSong(mod))
				if (devparm) I_OutputMsg("FMOD(Shutdown,FMUSIC_FreeSong): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
		}
		if (fmus)
		{
			if (fmod375->FSOUND_IsPlaying(fsoundchannel))
				if (!fmod375->FSOUND_Stream_Stop(fmus))
					if (devparm) I_OutputMsg("FMOD(Shutdown,FSOUND_Stream_Stop): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
			if (!fmod375->FSOUND_Stream_Close(fmus))
				if (devparm) I_OutputMsg("FMOD(Shutdown,FSOUND_Stream_Close): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
		}
		fmod375->FSOUND_Close();
#ifndef FMODMEMORY
		remove("fmod.tmp"); // Delete the temp file
#endif
		//if (!fmod375->FSOUND_StopSound(FSOUND_ALL))
			//if (fmod375->FSOUND_GetError() != FMOD_ERR_MEDIAPLAYER) CONS_Printf("FMOD(Shutdown,FSOUND_StopSound): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
		//fmod375->FMUSIC_StopAllSongs();
			//if (fmod375->FSOUND_GetError() != FMOD_ERR_NONE && fmod375->FSOUND_GetError() != FMOD_ERR_MEDIAPLAYER) CONS_Printf("FMOD(Shutdown,FMUSIC_StopAllSongs): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
	}
	FMOD_FreeInstance(fmod375);
	fmod375 = NULL;
	CONS_Printf(" Done\n");
}
#endif

void I_ShutdownDigMusic(void)
{
	nodigimusic = false;
	if (nomidimusic) I_ShutdownMusic();
}

#ifdef HAVE_MIXER
static boolean LoadSong(void *data, size_t lumplength, size_t selectpos)
{
	FILE *midfile;
	const char *tempname;
#ifdef USE_RWOPS
	if (canuseRW)
	{
		SDL_RWops *SDLRW;
		void *olddata = Smidi[selectpos]; //quick shortcut to set

		Z_Free(olddata); //free old memory
		Smidi[selectpos] = NULL;

		if (!data)
			return olddata != NULL; //was there old data?

		SDLRW = SDL_RWFromConstMem(data, (int)lumplength); //new RWops from Z_zone
		if (!SDLRW) //ERROR while making RWops!
		{
			CONS_Printf("Couldn't load music lump: %s\n", SDL_GetError());
			Z_Free(data);
			return false;
		}

		music[selectpos] = Mix_LoadMUS_RW(SDLRW); // new Mix_Chuck from RWops
		if (music[selectpos])
			Smidi[selectpos] = data; //all done
		else //ERROR while making Mix_Chuck
		{
			CONS_Printf("Couldn't load music data: %s\n", Mix_GetError());
			Z_Free(data);
			SDL_RWclose(SDLRW);
			Smidi[selectpos] = NULL;
		}
		return true;
	}
#endif
	tempname = va("%s/%s", MIDI_PATH, fmidi[selectpos]);

	if (!data)
	{
		if (FIL_FileExists(tempname))
			return unlink(tempname)+1;
#ifdef MIDI_PATH2
		else if (FIL_FileExists(tempname = va("%s/%s", MIDI_PATH2, fmidi[selectpos])))
			return unlink(tempname)+1;
#endif
		else
			return false;
	}

	midfile = fopen(tempname, "wb");

#ifdef MIDI_PATH2
	if (!midfile)
	{
		tempname = va("%s/%s", MIDI_PATH2, fmidi[selectpos]);
		midfile = fopen(tempname, "wb");
	}
#endif

	if (!midfile)
	{
		CONS_Printf("Couldn't open file %s to write music in\n", tempname);
		Z_Free(data);
		return false;
	}

	if (fwrite(data, lumplength, 1, midfile) == 0)
	{
		CONS_Printf("Couldn't write music into file %s because %s\n", tempname, strerror(ferror(midfile)));
		Z_Free(data);
		fclose(midfile);
		return false;
	}

	fclose(midfile);

	Z_Free(data);

	music[selectpos] = Mix_LoadMUS(tempname);
	if (!music[selectpos]) //ERROR while making Mix_Chuck
	{
		CONS_Printf("Couldn't load music file %s: %s\n", tempname, Mix_GetError());
		return false;
	}
	return true;
}
#endif


void I_ShutdownMusic(void)
{
#ifdef HAVE_MIXER
	if ((nomidimusic && nodigimusic) || !musicStarted)
		return;

	CONS_Printf("I_ShutdownMusic: ");

	I_UnRegisterSong(0);
	I_StopDigSong();
	Mix_CloseAudio();
#ifdef MIX_INIT
	Mix_Quit();
#endif
	CONS_Printf("shut down\n");
	musicStarted = SDL_FALSE;
	if (Msc_Mutex)
		SDL_DestroyMutex(Msc_Mutex);
	Msc_Mutex = NULL;
#endif
}

void I_InitMIDIMusic(void)
{
	if (nodigimusic) I_InitMusic();
}

static SDL_bool I_InitFMODMusic(void)
{
#ifdef FMODSOUND
#ifdef _WIN32
#ifdef _X86_
	char fmod375dll[] = "fmod375.dll";
	char fmod000dll[] = "fmod.dll";
#else
	char fmod375dll[] = "fmod64375.dll";
	char fmod000dll[] = "fmod64.dll";
#endif
#else
	char fmod375dll[] = "fmod375.so";
	char fmod000dll[] = "fmod.so";
#endif

	if (M_CheckParm("-nofmod"))
	{
		CONS_Printf(" disabled loading FMOD\n");
		return SDL_FALSE;
	}

	if (fmod375 && nofmodmusic)
	{
		FMOD_FreeInstance(fmod375);
		fmod375 = NULL;
	}

	fmod375 = FMOD_CreateInstance(fmod375dll);

	if(!fmod375)
		fmod375 = FMOD_CreateInstance(fmod000dll);

	if (fmod375)
		I_AddExitFunc(I_ShutdownFMODMusic);
	else
	{
		CONS_Printf(" failling loading FMOD\n");
		return SDL_FALSE;
	}

	if (fmod375)
	{
		// Tails 11-21-2002
		if (fmod375->FSOUND_GetVersion() < FMOD_VERSION)
		{
			//I_Error("FMOD Error : You are using the wrong DLL version!\nYou should be using FMOD %s\n", "FMOD_VERSION");
			CONS_Printf("FMOD Error : You are using the wrong DLL version!\nYou should be using FMOD %s\n", "FMOD_VERSION");
			return SDL_FALSE;
		}

		/*
			INITIALIZE
		*/
#if 0
		if (!fmod375->FSOUND_SetHWND(hWndMain))
		{
//			I_Error("FMOD(Init,FSOUND_SetHWND): %s\n", fmod375->FMOD_ErrorString(fmod375->FSOUND_GetError()));
			//fmod375->FSOUND_SetOutput(FSOUND_OUTPUT_DSOUND);
		}
		//else
#endif

		if (!fmod375->FSOUND_Init(44100, 32, FSOUND_INIT_DONTLATENCYADJUST))
		{
			CONS_Printf("FMOD(Init,FSOUND_Init): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
			return SDL_FALSE;
		}
		return SDL_TRUE;
	}
#endif
	return SDL_FALSE;
}

void I_InitDigMusic(void)
{
	if (nomidimusic) I_InitMusic();
}

void I_InitMusic(void)
{
#ifdef HAVE_MIXER
	char ad[100];
	SDL_version MIXcompiled;
	const SDL_version *MIXlinked;
#ifdef MIXER_INIT
	const int mixstart = MIX_INIT_OGG;
	int mixflags;
#endif
#endif
	if (I_InitFMODMusic())
	{
		nodigimusic = false;
		nofmodmusic = SDL_FALSE;
		return;
	}

	if ((nomidimusic && nodigimusic) || dedicated)
		return;

#ifdef HAVE_MIXER
	MIX_VERSION(&MIXcompiled)
	MIXlinked = Mix_Linked_Version();
	CONS_Printf("Compiled for SDL_mixer version: %d.%d.%d\n",
	            MIXcompiled.major, MIXcompiled.minor, MIXcompiled.patch);
#ifdef MIXER_POS
	if (MIXlinked->major == 1 && MIXlinked->minor == 2 && MIXlinked->patch < 7)
		canlooping = SDL_FALSE;
#endif
#ifdef USE_RWOPS
	if (M_CheckParm("-noRW"))
		canuseRW = SDL_FALSE;
#endif
	CONS_Printf("Linked with SDL_mixer version: %d.%d.%d\n",
	            MIXlinked->major, MIXlinked->minor, MIXlinked->patch);
#if !(defined (DC) || defined (PSP) || defined(GP2X))
	if (audio.freq < 44100 && !M_CheckParm ("-freq")) //I want atleast 44Khz
	{
		audio.samples = (Uint16)(audio.samples*(INT32)(44100/audio.freq));
		audio.freq = 44100; //Alam: to keep it around the same XX ms
	}
#endif

	if (sound_started
#ifdef HW3SOUND
		&& hws_mode == HWS_DEFAULT_MODE
#endif
		)
	{
		CONS_Printf("Temp Shutdown of SDL Audio System");
		SDL_CloseAudio();
		CONS_Printf(" Done\n");
	}

	CONS_Printf("I_InitMusic:");

#ifdef MIXER_INIT
	mixflags = Mix_Init(mixstart);
	if ((mixstart & MIX_INIT_FLAC) != (mixflags & MIX_INIT_FLAC))
	{
		CONS_Printf(" Unable to load FLAC support\n");
	}
	if ((mixstart & MIX_INIT_MOD ) != (mixflags & MIX_INIT_MOD ))
	{
		CONS_Printf(" Unable to load MOD support\n");
	}
	if ((mixstart & MIX_INIT_MP3 ) != (mixflags & MIX_INIT_MP3 ))
	{
		CONS_Printf(" Unable to load MP3 support\n");
	}
	if ((mixstart & MIX_INIT_OGG ) != (mixflags & MIX_INIT_OGG ))
	{
		CONS_Printf(" Unable to load OGG support\n");
		CONS_Printf(" This is bad news for you!\n");
	}
#endif

	if (Mix_OpenAudio(audio.freq, audio.format, audio.channels, audio.samples) < 0) //open_music(&audio)
	{
		CONS_Printf(" Unable to open music: %s\n", Mix_GetError());
		nomidimusic = nodigimusic = true;
		if (sound_started
#ifdef HW3SOUND
			&& hws_mode == HWS_DEFAULT_MODE
#endif
			)
		{
			if (SDL_OpenAudio(&audio, NULL) < 0) //retry
			{
				CONS_Printf(" couldn't reopen audio with desired format\n");
				nosound = true;
				sound_started = false;
			}
			else
			{
				CONS_Printf(" Restarting with audio driver : %s\n", SDL_AudioDriverName(ad, (int)sizeof ad));
			}
		}
		return;
	}
	else
		CONS_Printf(" Starting up with audio driver : %s with SDL_Mixer\n", SDL_AudioDriverName(ad, (int)sizeof ad));

	samplecount = audio.samples;
	CV_SetValue(&cv_samplerate, audio.freq);
	if (sound_started
#ifdef HW3SOUND
		&& hws_mode == HWS_DEFAULT_MODE
#endif
		)
		CONS_Printf(" Reconfigured SDL Audio System");
	else CONS_Printf(" Configured SDL_Mixer System");
	CONS_Printf(" with %d samples/slice at %ikhz(%dms buffer)\n", samplecount, audio.freq/1000, (INT32) ((audio.samples * 1000.0f) / audio.freq));
	Mix_SetPostMix(audio.callback, audio.userdata);  // after mixing music, add sound effects
	Mix_Resume(-1);
	CONS_Printf("I_InitMusic: music initialized\n");
	musicStarted = SDL_TRUE;
	Msc_Mutex = SDL_CreateMutex();
#endif
}

boolean I_PlaySong(INT32 handle, INT32 looping)
{
	(void)handle;
#ifdef HAVE_MIXER
	if (nomidimusic || !musicStarted || !music[handle])
		return false;

#ifdef MIXER_POS
	if (canlooping)
		Mix_HookMusicFinished(NULL);
#endif

	if (Mix_FadeInMusic(music[handle], looping ? -1 : 0, MIDIfade) == -1)
		CONS_Printf("I_PlaySong: Couldn't play song because %s\n", Mix_GetError());
	else
	{
		Mix_VolumeMusic(musicvol);
		return true;
	}
#else
	(void)looping;
#endif
	return false;
}

static void I_PauseFMOD(void)
{
#ifdef FMODSOUND
	if (!nofmodmusic && fmod375)
	{
		if (fmod375->FSOUND_GetError() != FMOD_ERR_NONE && fmod375->FSOUND_GetError() != FMOD_ERR_CHANNEL_ALLOC && fmod375->FSOUND_GetError() != FMOD_ERR_MEDIAPLAYER)
			if (devparm) I_OutputMsg("FMOD(Pause,Unknown): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
		if (mod)
		{
			if (!fmod375->FMUSIC_GetPaused(mod))
				if (!fmod375->FMUSIC_SetPaused(mod, true))
					if (devparm) I_OutputMsg("FMOD(Pause,FMUSIC_SetPaused): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
		}
		if (fmus)
		{
			if (!fmod375->FSOUND_GetPaused(fsoundchannel))
				if (!fmod375->FSOUND_SetPaused(fsoundchannel, true))
					if (devparm) I_OutputMsg("FMOD(Pause,FSOUND_SetPaused): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
		}
	}
#endif
}

void I_PauseSong(INT32 handle)
{
	(void)handle;
	I_PauseFMOD();
#ifdef HAVE_MIXER
	if ((nomidimusic && nodigimusic) || !musicStarted)
		return;

	Mix_PauseMusic();
	//I_StopSong(handle);
#endif
}

static void I_ResumeFMOD(void)
{
#ifdef FMODSOUND
	if (!nofmodmusic && fmod375)
	{
		if (fmod375->FSOUND_GetError() != FMOD_ERR_NONE && fmod375->FSOUND_GetError() != FMOD_ERR_CHANNEL_ALLOC && fmod375->FSOUND_GetError() != FMOD_ERR_MEDIAPLAYER)
			if (devparm) I_OutputMsg("FMOD(Resume,Unknown): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
		if (mod != NULL)
		{
			if (fmod375->FMUSIC_GetPaused(mod))
				if (!fmod375->FMUSIC_SetPaused(mod, false))
					if (devparm) I_OutputMsg("FMOD(Resume,FMUSIC_SetPaused): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
		}
		if (fmus != NULL)
		{
			if (fmod375->FSOUND_GetPaused(fsoundchannel))
				if (!fmod375->FSOUND_SetPaused(fsoundchannel, false))
					if (devparm) I_OutputMsg("FMOD(Resume,FSOUND_SetPaused): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
		}
	}
#endif
}

void I_ResumeSong(INT32 handle)
{
	(void)handle;
	I_ResumeFMOD();
#ifdef HAVE_MIXER
	if ((nomidimusic && nodigimusic) || !musicStarted)
		return;

	Mix_VolumeMusic(musicvol);
	Mix_ResumeMusic();
	//I_PlaySong(handle, true);
#endif
}

void I_StopSong(INT32 handle)
{
	(void)handle;
#ifdef HAVE_MIXER
	if (nomidimusic || !musicStarted)
		return;
	Mix_FadeOutMusic(MIDIfade);
#endif
}

void I_UnRegisterSong(INT32 handle)
{
	handle = 0;
#ifdef HAVE_MIXER

	if (nomidimusic || !musicStarted)
		return;

	Mix_HaltMusic();
	while (Mix_PlayingMusic())
		;

	if (music[handle])
		Mix_FreeMusic(music[handle]);
	music[handle] = NULL;
	LoadSong(NULL, 0, handle);
#endif
}

INT32 I_RegisterSong(void *data, size_t len)
{
#ifdef HAVE_MIXER
	if (nomidimusic || !musicStarted)
		return false;

	if (!LoadSong(data, len, 0))
		return false;

	if (music[0])
		return true;

	CONS_Printf("Couldn't load MIDI: %s\n", Mix_GetError());
#else
	(void)len;
	(void)data;
#endif
	return false;
}

void I_SetMIDIMusicVolume(INT32 volume)
{
#ifdef HAVE_MIXER
	if ((nomidimusic && nodigimusic) || !musicStarted)
		return;

	if (Msc_Mutex) SDL_LockMutex(Msc_Mutex);
	musicvol = volume * 2;
	if (Msc_Mutex) SDL_UnlockMutex(Msc_Mutex);
	Mix_VolumeMusic(musicvol);
#else
	(void)volume;
#endif
}

static boolean I_StartFMODSong(const char *musicname, INT32 looping)
{
#ifdef FMODSOUND
	char lumpname[9];
	static void *data = NULL;
	size_t len;
	lumpnum_t lumpnum = LUMPERROR;

	if (nofmodmusic)
		return false;

	if (fmod375->FSOUND_GetError() != FMOD_ERR_NONE && fmod375->FSOUND_GetError() != FMOD_ERR_CHANNEL_ALLOC &&
	    fmod375->FSOUND_GetError() != FMOD_ERR_MEDIAPLAYER && fmod375->FSOUND_GetError() != FMOD_ERR_INVALID_PARAM)
		if (devparm) I_OutputMsg("FMOD(Start,Unknown): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));

	if (fmus)
	{
		if (fmod375->FSOUND_IsPlaying(fsoundchannel))
			if (!fmod375->FSOUND_Stream_Stop(fmus))
				if (devparm) I_OutputMsg("FMOD(Start,FSOUND_Stream_Stop): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
		if (!fmod375->FSOUND_Stream_Close(fmus))
			if (devparm) I_OutputMsg("FMOD(Start,FSOUND_Stream_Close): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
		fsoundchannel = -1;
		fmus = NULL;
	}
	if (mod)
	{
		if (fmod375->FMUSIC_IsPlaying(mod))
			if (!fmod375->FMUSIC_StopSong(mod))
				if (devparm) I_OutputMsg("FMOD(Start,FMUSIC_StopSong): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
		if (!fmod375->FMUSIC_FreeSong(mod))
			if (devparm) I_OutputMsg("FMOD(Start,FMUSIC_FreeSong): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
		mod = NULL;
	}
	//if (!fmod375->FSOUND_StopSound(FSOUND_ALL))
		//if (fmod375->FSOUND_GetError() != FMOD_ERR_MEDIAPLAYER) CONS_Printf("FMOD(Start,FSOUND_StopSound): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
	//fmod375->FMUSIC_StopAllSongs();
	//if (fmod375->FSOUND_GetError() != FMOD_ERR_NONE && fmod375->FSOUND_GetError() != FMOD_ERR_MEDIAPLAYER) CONS_Printf("FMOD(Start,FMUSIC_StopAllSongs): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));

	// Create the lumpname we need
	sprintf(lumpname, "o_%s", musicname);

	lumpnum = W_CheckNumForName(lumpname);

	if (lumpnum == LUMPERROR)
	{
		sprintf(lumpname, "d_%s", musicname);

		lumpnum = W_CheckNumForName(lumpname);

		if (lumpnum == LUMPERROR) // Graue 02-29-2004: don't worry about missing music, there might still be a MIDI
			return false; // No music found. Oh well!
	}

#ifndef FMODMEMORY
	Z_Free(data);
#endif
	len = W_LumpLength(lumpnum);
	data = W_CacheLumpNum(lumpnum, PU_MUSIC);

#ifdef FMODMEMORY
	mod = fmod375->FMUSIC_LoadSongEx(data, 0, (INT32)len, ((looping) ? (FSOUND_LOOP_NORMAL) : (0))|FSOUND_LOADMEMORY, NULL, 0);
#else
	I_SaveMemToFile(data, len, "fmod.tmp");

	mod = fmod375->FMUSIC_LoadSong("fmod.tmp");
#endif

	if (fmod375->FSOUND_GetError() != FMOD_ERR_NONE)
	{
		if (fmod375->FSOUND_GetError() != FMOD_ERR_FILE_FORMAT)
			if (devparm) I_OutputMsg("FMOD(Start,FMUSIC_LoadSong): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));

		if (mod)
		{
			if (fmod375->FMUSIC_IsPlaying(mod))
				if (!fmod375->FMUSIC_StopSong(mod))
					if (devparm) I_OutputMsg("FMOD(Start,FMUSIC_StopSong): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
			if (!fmod375->FMUSIC_FreeSong(mod))
				if (devparm) I_OutputMsg("FMOD(Start,FMUSIC_FreeSong): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
			mod = NULL;
		}
	}

	if (mod)
	{
		if (!fmod375->FMUSIC_SetLooping(mod, (signed char)looping))
		{
			if (devparm) I_OutputMsg("FMOD(Start,FMUSIC_SetLooping): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
		}
//		else if (fmod375->FMUSIC_GetType(mod) == FMUSIC_TYPE_MOD || fmod375->FMUSIC_GetType(mod) == FMUSIC_TYPE_S3M)
//		{
//			if (!fmod375->FMUSIC_SetPanSeperation(mod, 0.85f))		/* 15% crossover */
//				CONS_Printf("FMOD(Start,FMUSIC_SetPanSeperation): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
//		}
		else if (!fmod375->FMUSIC_SetPanSeperation(mod, 0.0f))
		{
			if (devparm) I_OutputMsg("FMOD(Start,FMUSIC_SetPanSeperation): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
		}
	}
	else
	{
#ifdef FMODMEMORY
		fmus = fmod375->FSOUND_Stream_Open(data, ((looping) ? (FSOUND_LOOP_NORMAL) : (0))|FSOUND_LOADMEMORY, 0, (INT32)len);
#else
		fmus = fmod375->FSOUND_Stream_Open("fmod.tmp", ((looping) ? (FSOUND_LOOP_NORMAL) : (0)), 0, len);
#endif
		if (fmus == NULL)
		{
			if (devparm) I_OutputMsg("FMOD(Start,FSOUND_Stream_Open): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
			return false;
		}
	}

	// Scan the Ogg Vorbis file for the COMMENT= field for a custom loop point
	if (fmus && looping)
	{
		const char *dataum = data;
		size_t scan;
		unsigned int loopstart = 0;
		UINT8 newcount = 0;
		char looplength[64];

		for (scan = 0;scan < len; scan++)
		{
			if (*dataum++ == 'C'){
			if (*dataum++ == 'O'){
			if (*dataum++ == 'M'){
			if (*dataum++ == 'M'){
			if (*dataum++ == 'E'){
			if (*dataum++ == 'N'){
			if (*dataum++ == 'T'){
			if (*dataum++ == '='){
			if (*dataum++ == 'L'){
			if (*dataum++ == 'O'){
			if (*dataum++ == 'O'){
			if (*dataum++ == 'P'){
			if (*dataum++ == 'P'){
			if (*dataum++ == 'O'){
			if (*dataum++ == 'I'){
			if (*dataum++ == 'N'){
			if (*dataum++ == 'T'){
			if (*dataum++ == '=')
			{
				while (*dataum != 1 && newcount != 63)
					looplength[newcount++] = *dataum++;

				looplength[newcount] = '\0';

				loopstart = atoi(looplength);
			}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
		}

		if (loopstart > 0)
		{
			const INT32 length = fmod375->FSOUND_Stream_GetLengthMs(fmus);
			const INT32 freq = 44100; //= FSOUND_GetFrequency(fsoundchannel);
			const UINT32 loopend = (UINT32)((freq/1000.0f)*length-(freq/1000.0f));
			//const UINT32 loopend = (((freq/2)*length)/500)-8;
			if (!fmod375->FSOUND_Stream_SetLoopPoints(fmus, loopstart, loopend) && devparm)
				I_OutputMsg("FMOD(Start,FSOUND_Stream_SetLoopPoints): %s\n",
					FMOD_ErrorString(fmod375->FSOUND_GetError()));
		}
	}

	/*
		PLAY SONG
	*/
	if (mod)
	{
		if (fmod375->FMUSIC_PlaySong(mod))
		{
			fsoundchannel = -1;
			I_SetDigMusicVolume(-1);
			return true;
		}
		else
		{
			if (devparm)
				I_OutputMsg("FMOD(Start,FMUSIC_PlaySong): %s\n",
				            FMOD_ErrorString(fmod375->FSOUND_GetError()));
			return false;
		}
	}
	if (fmus)
	{
		fsoundchannel = fmod375->FSOUND_Stream_PlayEx(FSOUND_FREE, fmus, NULL, TRUE);

		if (fsoundchannel == -1)
		{
			if (devparm)
				I_OutputMsg("FMOD(Start,FSOUND_Stream_PlayEx): %s\n",
				            FMOD_ErrorString(fmod375->FSOUND_GetError()));
			return false;
		}
		else if (!fmod375->FSOUND_SetPaused(fsoundchannel, FALSE))
		{
			if (devparm)
				I_OutputMsg("FMOD(Start,FSOUND_SetPaused): %s\n",
					FMOD_ErrorString(fmod375->FSOUND_GetError()));
			return false;
		}

		I_SetDigMusicVolume(-1);
		fsoundfreq = fmod375->FSOUND_GetFrequency(fsoundchannel);
		return true;
	}
#else
	(void)musicname;
	(void)looping;
#endif
	return false;
}

boolean I_StartDigSong(const char *musicname, INT32 looping)
{
#ifdef HAVE_MIXER
	XBOXSTATIC char filename[9];
	void *data;
	lumpnum_t lumpnum;
	size_t lumplength;
#endif

	if(I_StartFMODSong(musicname, looping))
		return true;

#ifdef HAVE_MIXER
	if (nodigimusic)
		return false;

	snprintf(filename, sizeof filename, "o_%s", musicname);

	lumpnum = W_CheckNumForName(filename);

	I_StopDigSong();

	if (lumpnum == LUMPERROR)
	{
		// Alam_GBC: like in win32/win_snd.c: Graue 02-29-2004: don't worry about missing music, there might still be a MIDI
		//CONS_Printf("Music lump %s not found!\n", filename);
		return false; // No music found. Oh well!
	}
	else
		lumplength = W_LumpLength(lumpnum);

	data = W_CacheLumpNum(lumpnum, PU_MUSIC);

	if (Msc_Mutex) SDL_LockMutex(Msc_Mutex);

#ifdef MIXER_POS
	if (canlooping && (loopingDig = looping) == SDL_TRUE && strcmp(data, "OggS")  == 0)
		looping = false; // Only on looping Ogg files, will we will do our own looping

	// Scan the Ogg Vorbis file for the COMMENT= field for a custom
	// loop point
	if (!looping && loopingDig)
	{
		size_t scan;
		const char *dataum = data;
		XBOXSTATIC char looplength[64];
		UINT32 loopstart = 0;
		UINT8 newcount = 0;

		Mix_HookMusicFinished(I_FinishMusic);

		for (scan = 0; scan < lumplength; scan++)
		{
			if (*dataum++ == 'C'){
			if (*dataum++ == 'O'){
			if (*dataum++ == 'M'){
			if (*dataum++ == 'M'){
			if (*dataum++ == 'E'){
			if (*dataum++ == 'N'){
			if (*dataum++ == 'T'){
			if (*dataum++ == '='){
			if (*dataum++ == 'L'){
			if (*dataum++ == 'O'){
			if (*dataum++ == 'O'){
			if (*dataum++ == 'P'){
			if (*dataum++ == 'P'){
			if (*dataum++ == 'O'){
			if (*dataum++ == 'I'){
			if (*dataum++ == 'N'){
			if (*dataum++ == 'T'){
			if (*dataum++ == '=')
			{

				while (*dataum != 1 && newcount != 63)
					looplength[newcount++] = *dataum++;

				looplength[newcount] = '\0';

				loopstart = atoi(looplength);

			}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
			else
				dataum--;}
		}

		if (loopstart > 0)
		{
			loopstartDig = (double)((44.1l+loopstart) / 44100.0l); //8 PCM chucks off and PCM to secs
//#ifdef GP2X//#ifdef PARANOIA
			//I_OutputMsg("I_StartDigSong: setting looping point to %ul PCMs(%f seconds)\n", loopstart, loopstartDig);
//#endif
		}
		else
		{
			looping = true; // loopingDig true, but couldn't find start loop point
		}
	}
	else
		loopstartDig = 0.0l;
#else
	if (looping && strcmp(data, "OggS")  == 0)
		I_OutputMsg("I_StartDigSong: SRB2 was not compiled with looping music support(no Mix_FadeInMusicPos)\n");
#endif

	if (!LoadSong(data, lumplength, 1))
	{
		if (Msc_Mutex) SDL_UnlockMutex(Msc_Mutex);
		return false;
	}

	// Note: LoadSong() frees the data. Let's make sure
	// we don't try to use the data again.
	data = NULL;

	if (Mix_FadeInMusic(music[1], looping ? -1 : 0, Digfade) == -1)
	{
		if (Msc_Mutex) SDL_UnlockMutex(Msc_Mutex);
		if (devparm)
			I_OutputMsg("I_StartDigSong: Couldn't play song %s because %s\n", musicname, Mix_GetError());
		return false;
	}
	Mix_VolumeMusic(musicvol);

	if (Msc_Mutex) SDL_UnlockMutex(Msc_Mutex);
	return true;
#else
	(void)looping;
	(void)musicname;
	return false;
#endif
}

static void I_StopFMOD(void)
{
#ifdef FMODSOUND
	if (!nofmodmusic)
	{
		if (fmod375->FSOUND_GetError() != FMOD_ERR_NONE && fmod375->FSOUND_GetError() != FMOD_ERR_INVALID_PARAM && fmod375->FSOUND_GetError() != FMOD_ERR_CHANNEL_ALLOC && fmod375->FSOUND_GetError() != FMOD_ERR_MEDIAPLAYER)
			if (devparm) I_OutputMsg("FMOD(Stop,Unknown): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
		if (mod)
		{
			if (fmod375->FMUSIC_IsPlaying(mod))
			{
				if (!fmod375->FMUSIC_StopSong(mod))
					if (devparm) I_OutputMsg("FMOD(Stop,FMUSIC_StopSong): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
			}
		}
		if (fmus)
		{
			if (fmod375->FSOUND_IsPlaying(fsoundchannel))
			{
				if (!fmod375->FSOUND_Stream_Stop(fmus))
					if (devparm) I_OutputMsg("FMOD(Stop,FSOUND_Stream_Stop): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
			}
		}
		//if (!fmod375->FSOUND_StopSound(FSOUND_ALL))
			//if (fmod375->FSOUND_GetError() != FMOD_ERR_MEDIAPLAYER) CONS_Printf("FMOD(Stop,FSOUND_StopSound): %s\n", fmod375->FMOD_ErrorString(FSOUND_GetError()));
		//fmod375->FMUSIC_StopAllSongs();
			//if (fmod375->FSOUND_GetError() != FMOD_ERR_NONE && fmod375->FSOUND_GetError() != FMOD_ERR_MEDIAPLAYER) CONS_Printf("FMOD(Stop,FMUSIC_StopAllSongs): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
	}
#endif
}

void I_StopDigSong(void)
{
	I_StopFMOD();
#ifdef HAVE_MIXER
	if (nodigimusic)
		return;

#ifdef MIXER_POS
	if (canlooping)
		Mix_HookMusicFinished(NULL);
#endif

	Mix_HaltMusic();
	while (Mix_PlayingMusic())
		;

	if (music[1])
		Mix_FreeMusic(music[1]);
	music[1] = NULL;
	LoadSong(NULL, 0, 1);
#endif
}


static void I_SetFMODMusicVolume(INT32 volume)
{
#ifdef FMODSOUND
	if (volume != -1)
		fmodvol = (volume<<3)+(volume>>2);
	if (!nofmodmusic)
	{
		if (fmod375->FSOUND_GetError() != FMOD_ERR_NONE && fmod375->FSOUND_GetError() != FMOD_ERR_CHANNEL_ALLOC && fmod375->FSOUND_GetError() != FMOD_ERR_MEDIAPLAYER)
			if (devparm) I_OutputMsg("FMOD(Volume,Unknown): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
		if (mod)
		{
			if (fmod375->FMUSIC_GetType(mod) != FMUSIC_TYPE_NONE)
			{
				if (!fmod375->FMUSIC_SetMasterVolume(mod, fmodvol) && devparm)
					I_OutputMsg("FMOD(Volume,FMUSIC_SetMasterVolume): %s\n",
						FMOD_ErrorString(fmod375->FSOUND_GetError()));
			}
			else if (devparm)
				I_OutputMsg("FMOD(Volume,FMUSIC_GetType): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
		}
		if (fmus)
		{
			if (!fmod375->FSOUND_SetVolume(fsoundchannel, fmodvol))
				if (devparm) I_OutputMsg("FMOD(Volume,FSOUND_SetVolume): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
		}
	}
#else
	(void)volume;
#endif
}

void I_SetDigMusicVolume(INT32 volume)
{
	I_SetFMODMusicVolume(volume);

	I_SetMIDIMusicVolume(volume);
}

boolean I_SetSongSpeed(float speed)
{
#ifdef FMODSOUND
	if (nofmodmusic)
		return false; //there no music or FMOD is not loaded

	if ((!fmus || !fmod375->FSOUND_IsPlaying(fsoundchannel)) && (!mod || !fmod375->FMUSIC_IsPlaying(mod)))
		return false; //there no FMOD music playing

	if (speed == 0.0f)
		return true; //yes, we can set the speed
	if (speed > 250.0f)
		speed = 250.0f; //limit speed up to 250x

	if (fmus && fmod375->FSOUND_IsPlaying(fsoundchannel))
	{
		if (!fmod375->FSOUND_SetFrequency(fsoundchannel,(INT32)(speed*fsoundfreq)))
		{
			if (devparm)
				I_OutputMsg("FMOD(ChangeSpeed,FSOUND_SetFrequency): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
		}
		else
			return true;
	}
	else if (mod && fmod375->FMUSIC_IsPlaying(mod))
	{
		if (fmod375->FMUSIC_SetMasterSpeed(mod, speed))
		{
			if (devparm)
				I_OutputMsg("FMOD(ChangeSpeed,FMUSIC_SetMasterSpeed): %s\n", FMOD_ErrorString(fmod375->FSOUND_GetError()));
		}
		else
			return true;
	}
#else
	(void)speed;
#endif
	return false;
}

#ifdef MIXER_POS
static void SDLCALL I_FinishMusic(void)
{
	if (!music[1])
		return;
	else if (Msc_Mutex) SDL_LockMutex(Msc_Mutex);
	//I_OutputMsg("I_FinishMusic: Loopping song to %g seconds\n", loopstartDig);
	if (Mix_FadeInMusicPos(music[1], loopstartDig ? 0 : -1, Digfade,
		loopstartDig) == 0)
	{
		Mix_VolumeMusic(musicvol);
	}
	else if (devparm)
	{
		I_OutputMsg("I_FinishMusic: Couldn't loop song because %s\n",
			Mix_GetError());
	}
	if (Msc_Mutex) SDL_UnlockMutex(Msc_Mutex);
}
#endif
#endif //SDL
