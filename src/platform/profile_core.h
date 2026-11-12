//
// profile_core.h
//
// BM-DOS - where core 0's time goes, in one-second windows.  Fixed parts the
// port times itself (presenting video, the audio queue with the MIDI mix, USB
// input, the metronome's waits, the whole emulator call) and DOSBox's sound
// channels by name ("FM", "SB", "GUS", "SPKR"...), timed in
// MixerChannel::Mix by patches/dosbox-core/mixer-profile.patch.  The x86 CPU
// is what is left of the emulator call once the work done inside it (sound
// channels, the video and audio callbacks) is taken out.
//
// Item 5 of the list: to decide whether the sound should move to a free core.
// No Circle: scripts/hostcheck/profile_test.cpp (make check-profile).
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#ifndef BARE_PROFILE_CORE_H
#define BARE_PROFILE_CORE_H

#include <cstdint>
#include <cstring>

namespace bareprofile {

enum TPart : unsigned
{
	PartCall,		// the emulator call (retro_run), everything inside it
	PartWait,		// the metronome's waits (inside the call)
	PartVideo,		// presenting a frame (inside the call: video callback)
	PartAudio,		// the audio queue, MIDI mix included (inside: audio callback)
	PartInput,		// USB input (inside the call: the core's input poll)
	PartCount
};

constexpr unsigned MaxChannels = 12;

struct TSnapshot
{
	std::uint64_t nWindowTicks = 0;		// the window's length
	std::uint64_t Parts[PartCount] = {};
	unsigned nChannels = 0;
	const char *ChannelNames[MaxChannels] = {};
	std::uint64_t Channels[MaxChannels] = {};

	/// The x86 CPU: the call less what was done inside it.
	std::uint64_t Cpu (void) const
	{
		std::uint64_t nInside = Parts[PartWait] + Parts[PartVideo] + Parts[PartAudio]
					+ Parts[PartInput];
		for (unsigned i = 0; i < nChannels; i++) nInside += Channels[i];
		return Parts[PartCall] > nInside ? Parts[PartCall] - nInside : 0;
	}

	std::uint64_t Sound (void) const
	{
		std::uint64_t n = 0;
		for (unsigned i = 0; i < nChannels; i++) n += Channels[i];
		return n;
	}

	/// Tenths of a percent of the window.
	unsigned Permille (std::uint64_t nTicks) const
	{
		return nWindowTicks != 0 ? static_cast<unsigned> (nTicks * 1000 / nWindowTicks) : 0;
	}
};

class CProfile
{
public:
	void Add (TPart Part, std::uint64_t nTicks) { m_Now.Parts[Part] += nTicks; }

	/// A sound channel's time.  Names are DOSBox's own string constants, so the
	/// pointer identifies the channel; an unseen one gets a slot while there is
	/// one left.
	void AddChannel (const char *pName, std::uint64_t nTicks)
	{
		for (unsigned i = 0; i < m_Now.nChannels; i++)
		{
			if (m_Now.ChannelNames[i] == pName || (pName && m_Now.ChannelNames[i]
			    && std::strcmp (m_Now.ChannelNames[i], pName) == 0))
			{
				m_Now.Channels[i] += nTicks;
				return;
			}
		}
		if (m_Now.nChannels < MaxChannels)
		{
			m_Now.ChannelNames[m_Now.nChannels] = pName;
			m_Now.Channels[m_Now.nChannels++] = nTicks;
		}
	}

	/// Called often (each loop); closes a window every nWindowTicks.
	void Tick (std::uint64_t nNow, std::uint64_t nWindowTicks)
	{
		if (!m_bStarted) { m_bStarted = true; m_nStart = nNow; return; }
		if (nNow - m_nStart < nWindowTicks) return;
		m_Now.nWindowTicks = nNow - m_nStart;
		m_Last = m_Now;
		// Keep the channel names (and their order) for the next window.
		TSnapshot Next;
		Next.nChannels = m_Now.nChannels;
		for (unsigned i = 0; i < m_Now.nChannels; i++) Next.ChannelNames[i] = m_Now.ChannelNames[i];
		m_Now = Next;
		m_nStart = nNow;
	}

	/// A pause (menu, Alt+Pause, DOS restart): the window in progress is not a
	/// fair sample - drop it, keep the last complete one.
	void Restart (std::uint64_t nNow)
	{
		TSnapshot Next;
		Next.nChannels = m_Now.nChannels;
		for (unsigned i = 0; i < m_Now.nChannels; i++) Next.ChannelNames[i] = m_Now.ChannelNames[i];
		m_Now = Next;
		m_nStart = nNow;
		m_bStarted = true;
	}

	const TSnapshot &Last (void) const { return m_Last; }

private:
	TSnapshot m_Now;
	TSnapshot m_Last;
	std::uint64_t m_nStart = 0;
	bool m_bStarted = false;
};

} // namespace bareprofile

#endif
