//
// audio_dcblock.h
//
// BM-DOS - a DC blocker for the final stereo output.
//
// A real sound card is AC-coupled: a capacitor on the line output removes any
// constant level, so a PC speaker left "on", a Sound Blaster DAC parked at 0x00
// or a GUS voice stopped mid-wave is silent.  DOSBox's mixer is not, and on the
// board that showed as a constant -0.8 of full scale in DOOM's SETUP screen.
// On its own a constant level is inaudible; the moment the output has a gap
// (the emulator running behind real time, the device playing zeros for a few
// milliseconds) it turns into a square wave at the frame rate - a loud buzz
// "in sync with the screen".
//
// This is the classic one-pole high-pass, y[n] = x[n] - x[n-1] + R * y[n-1],
// per channel, with the corner at about 10 Hz: below anything a DOS game plays,
// and far enough to let a held level decay in a few tenths of a second.  No
// Circle dependency: scripts/hostcheck/dcblock_test.cpp runs it on the PC.
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#ifndef BARE_AUDIO_DCBLOCK_H
#define BARE_AUDIO_DCBLOCK_H

#include <cstdint>

namespace bareaudio {

class CDCBlocker
{
public:
	/// \param fCornerHz  -3 dB point; \param fSampleRate  output rate
	explicit CDCBlocker (float fCornerHz = 10.0f, float fSampleRate = 44100.0f)
	: m_fR (1.0f - 2.0f * 3.14159265f * fCornerHz / fSampleRate)
	{
	}

	/// Filter interleaved stereo s16 in place, saturating.
	void Process (std::int16_t *pFrames, unsigned nFrames)
	{
		for (unsigned i = 0; i < nFrames; i++)
		{
			for (unsigned c = 0; c < 2; c++)
			{
				const float x = pFrames[2 * i + c];
				const float y = x - m_fX[c] + m_fR * m_fY[c];
				m_fX[c] = x;
				m_fY[c] = y;

				const float s = y > 32767.0f ? 32767.0f : y < -32768.0f ? -32768.0f : y;
				pFrames[2 * i + c] = static_cast<std::int16_t> (s);
			}
		}
	}

	void Reset (void)
	{
		m_fX[0] = m_fX[1] = m_fY[0] = m_fY[1] = 0.0f;
	}

private:
	float m_fR;
	float m_fX[2] = { 0.0f, 0.0f };
	float m_fY[2] = { 0.0f, 0.0f };
};

} // namespace bareaudio

#endif
