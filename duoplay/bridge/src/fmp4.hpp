// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace duo {

// ISO/IEC 23091-2 colour description written as an MP4 'colr' box (0 = leave it to the stream).
struct Fmp4Color {
	uint16_t primaries = 0, transfer = 0, matrix = 0;
	bool fullRange = false;
};

// Wraps the console's video frames, untouched, as fragmented MP4 for Safari's media
// player (the HDR path): one init segment, then one tiny fragment per frame.
class Fmp4Muxer {
public:
	using Color = Fmp4Color;

	Fmp4Muxer(bool hevc, int fps, Color color = Color()) : mHevc(hevc), mFps(fps > 0 ? fps : 60), mColor(color) {}

	// Feed a keyframe (Annex-B). Returns true when its parameter sets differ from the
	// current ones, i.e. a (new) init segment must be sent before the next fragment.
	bool configure(const uint8_t *au, size_t size);
	bool ready() const { return mReady; }

	std::vector<uint8_t> initSegment() const;
	std::string mime() const;     // e.g. video/mp4; codecs="hvc1.2.4.L153.B0"
	int width() const { return mWidth; }
	int height() const { return mHeight; }

	std::vector<uint8_t> fragment(const uint8_t *au, size_t size, bool keyframe);

private:
	bool parseSps(const std::vector<uint8_t> &sps);
	std::vector<uint8_t> configRecord() const;

	bool mHevc;
	int mFps;
	Color mColor;
	bool mReady = false;
	std::vector<uint8_t> mVps, mSps, mPps;
	int mWidth = 0, mHeight = 0;
	// HEVC profile_tier_level / AVC profile fields
	uint8_t mProfileSpace = 0, mTier = 0, mProfileIdc = 0, mLevelIdc = 0;
	uint32_t mCompatFlags = 0;
	uint8_t mConstraint[6] = {0};
	uint8_t mChromaFormat = 1, mBitDepthLuma = 8, mBitDepthChroma = 8;
	uint8_t mMaxSubLayers = 1, mTemporalIdNested = 0;
	uint8_t mAvcConstraint = 0;
	uint32_t mSequence = 0;
	uint64_t mDecodeTime = 0;
};

} // namespace duo
