// SPDX-License-Identifier: AGPL-3.0-only
#include "fmp4.hpp"

#include <cstdio>
#include <cstring>

namespace duo {

namespace {

struct Nal {
	const uint8_t *data; // first byte of the NAL header
	size_t size;
	int type;
};

std::vector<Nal> splitNals(const uint8_t *d, size_t n, bool hevc) {
	std::vector<size_t> starts, payloads;
	for (size_t i = 0; i + 3 <= n; i++) {
		if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
			starts.push_back(i > 0 && d[i - 1] == 0 ? i - 1 : i);
			payloads.push_back(i + 3);
			i += 2;
		}
	}
	std::vector<Nal> out;
	for (size_t k = 0; k < payloads.size(); k++) {
		size_t b = payloads[k];
		size_t e = k + 1 < starts.size() ? starts[k + 1] : n;
		while (e > b && d[e - 1] == 0) // trailing zero bytes belong to the next start code
			e--;
		if (e <= b)
			continue;
		int t = hevc ? (d[b] >> 1) & 0x3f : d[b] & 0x1f;
		out.push_back({d + b, e - b, t});
	}
	return out;
}

std::vector<uint8_t> unescape(const uint8_t *d, size_t n) {
	std::vector<uint8_t> out;
	out.reserve(n);
	for (size_t i = 0; i < n; i++) {
		if (i + 2 < n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 3) {
			out.push_back(0);
			out.push_back(0);
			i += 2;
			continue;
		}
		out.push_back(d[i]);
	}
	return out;
}

class Bits {
public:
	Bits(const std::vector<uint8_t> &d, size_t startByte) : mD(d), mPos(startByte * 8) {}
	uint32_t u(int n) {
		uint32_t v = 0;
		for (int i = 0; i < n; i++) {
			size_t byte = mPos >> 3;
			int bit = 7 - int(mPos & 7);
			v = (v << 1) | (byte < mD.size() ? (mD[byte] >> bit) & 1 : 0);
			mPos++;
		}
		return v;
	}
	uint32_t ue() {
		int zeros = 0;
		while (u(1) == 0 && zeros < 32)
			zeros++;
		return zeros ? ((1u << zeros) - 1 + u(zeros)) : 0;
	}
	int32_t se() {
		uint32_t k = ue();
		return (k & 1) ? int32_t((k + 1) / 2) : -int32_t(k / 2);
	}
	void skip(int n) { mPos += size_t(n); }
	bool ok() const { return (mPos >> 3) <= mD.size(); }

private:
	const std::vector<uint8_t> &mD;
	size_t mPos;
};

// ---- box writing ---------------------------------------------------------------------
struct Buf {
	std::vector<uint8_t> v;
	void u8(uint8_t x) { v.push_back(x); }
	void u16(uint16_t x) { u8(uint8_t(x >> 8)); u8(uint8_t(x)); }
	void u24(uint32_t x) { u8(uint8_t(x >> 16)); u16(uint16_t(x)); }
	void u32(uint32_t x) { u16(uint16_t(x >> 16)); u16(uint16_t(x)); }
	void u64(uint64_t x) { u32(uint32_t(x >> 32)); u32(uint32_t(x)); }
	void fourcc(const char *s) { v.insert(v.end(), s, s + 4); }
	void bytes(const std::vector<uint8_t> &b) { v.insert(v.end(), b.begin(), b.end()); }
	void bytes(const uint8_t *b, size_t n) { v.insert(v.end(), b, b + n); }
	void zeros(size_t n) { v.insert(v.end(), n, 0); }
	size_t begin(const char *type) {
		size_t at = v.size();
		u32(0);
		fourcc(type);
		return at;
	}
	size_t full(const char *type, uint8_t version, uint32_t flags) {
		size_t at = begin(type);
		u8(version);
		u24(flags);
		return at;
	}
	void end(size_t at) {
		uint32_t size = uint32_t(v.size() - at);
		v[at] = uint8_t(size >> 24);
		v[at + 1] = uint8_t(size >> 16);
		v[at + 2] = uint8_t(size >> 8);
		v[at + 3] = uint8_t(size);
	}
};

void matrix(Buf &b) {
	const uint32_t m[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
	for (uint32_t x : m)
		b.u32(x);
}

} // namespace

bool Fmp4Muxer::configure(const uint8_t *au, size_t size) {
	std::vector<uint8_t> vps, sps, pps;
	for (const auto &nal : splitNals(au, size, mHevc)) {
		std::vector<uint8_t> bytes(nal.data, nal.data + nal.size);
		if (mHevc) {
			if (nal.type == 32 && vps.empty())
				vps = bytes;
			else if (nal.type == 33 && sps.empty())
				sps = bytes;
			else if (nal.type == 34 && pps.empty())
				pps = bytes;
		} else {
			if (nal.type == 7 && sps.empty())
				sps = bytes;
			else if (nal.type == 8 && pps.empty())
				pps = bytes;
		}
	}
	if (sps.empty() || pps.empty() || (mHevc && vps.empty()))
		return false;
	if (mReady && sps == mSps && pps == mPps && vps == mVps)
		return false;
	if (!parseSps(sps))
		return false;
	mVps = vps;
	mSps = sps;
	mPps = pps;
	mReady = true;
	return true;
}

bool Fmp4Muxer::parseSps(const std::vector<uint8_t> &nal) {
	if (mHevc) {
		auto r = unescape(nal.data() + 2, nal.size() - 2);
		Bits b(r, 0);
		b.u(4); // sps_video_parameter_set_id
		int maxSubLayersMinus1 = int(b.u(3));
		mMaxSubLayers = uint8_t(maxSubLayersMinus1 + 1);
		mTemporalIdNested = uint8_t(b.u(1));
		mProfileSpace = uint8_t(b.u(2));
		mTier = uint8_t(b.u(1));
		mProfileIdc = uint8_t(b.u(5));
		mCompatFlags = b.u(32);
		for (auto &c : mConstraint)
			c = uint8_t(b.u(8));
		mLevelIdc = uint8_t(b.u(8));
		std::vector<int> profPresent(8, 0), levelPresent(8, 0);
		for (int i = 0; i < maxSubLayersMinus1; i++) {
			profPresent[i] = int(b.u(1));
			levelPresent[i] = int(b.u(1));
		}
		if (maxSubLayersMinus1 > 0)
			for (int i = maxSubLayersMinus1; i < 8; i++)
				b.u(2);
		for (int i = 0; i < maxSubLayersMinus1; i++) {
			if (profPresent[i])
				b.skip(88);
			if (levelPresent[i])
				b.skip(8);
		}
		b.ue(); // sps_seq_parameter_set_id
		mChromaFormat = uint8_t(b.ue());
		if (mChromaFormat == 3)
			b.u(1);
		uint32_t w = b.ue(), h = b.ue();
		if (b.u(1)) { // conformance window
			uint32_t l = b.ue(), rr = b.ue(), t = b.ue(), bo = b.ue();
			int subW = (mChromaFormat == 1 || mChromaFormat == 2) ? 2 : 1;
			int subH = mChromaFormat == 1 ? 2 : 1;
			w -= uint32_t(subW) * (l + rr);
			h -= uint32_t(subH) * (t + bo);
		}
		mBitDepthLuma = uint8_t(b.ue() + 8);
		mBitDepthChroma = uint8_t(b.ue() + 8);
		mWidth = int(w);
		mHeight = int(h);
		return b.ok() && w > 0 && h > 0 && w < 16384 && h < 16384;
	}

	auto r = unescape(nal.data() + 1, nal.size() - 1);
	Bits b(r, 0);
	mProfileIdc = uint8_t(b.u(8));
	mAvcConstraint = uint8_t(b.u(8));
	mLevelIdc = uint8_t(b.u(8));
	b.ue(); // seq_parameter_set_id
	int chroma = 1;
	static const int high[] = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135};
	bool isHigh = false;
	for (int p : high)
		isHigh |= p == mProfileIdc;
	if (isHigh) {
		chroma = int(b.ue());
		if (chroma == 3)
			b.u(1);
		mBitDepthLuma = uint8_t(b.ue() + 8);
		mBitDepthChroma = uint8_t(b.ue() + 8);
		b.u(1); // qpprime_y_zero_transform_bypass
		if (b.u(1)) { // seq_scaling_matrix_present
			for (int i = 0; i < (chroma != 3 ? 8 : 12); i++) {
				if (!b.u(1))
					continue;
				int size = i < 6 ? 16 : 64, last = 8, next = 8;
				for (int j = 0; j < size; j++) {
					if (next != 0)
						next = (last + b.se() + 256) % 256;
					last = next == 0 ? last : next;
				}
			}
		}
	}
	mChromaFormat = uint8_t(chroma);
	b.ue(); // log2_max_frame_num_minus4
	uint32_t pocType = b.ue();
	if (pocType == 0) {
		b.ue();
	} else if (pocType == 1) {
		b.u(1);
		b.se();
		b.se();
		uint32_t n = b.ue();
		for (uint32_t i = 0; i < n && i < 256; i++)
			b.se();
	}
	b.ue(); // max_num_ref_frames
	b.u(1);
	uint32_t pw = b.ue(), ph = b.ue();
	uint32_t frameMbsOnly = b.u(1);
	if (!frameMbsOnly)
		b.u(1);
	b.u(1); // direct_8x8_inference
	uint32_t w = (pw + 1) * 16, h = (2 - frameMbsOnly) * (ph + 1) * 16;
	if (b.u(1)) { // frame_cropping
		uint32_t l = b.ue(), rr = b.ue(), t = b.ue(), bo = b.ue();
		uint32_t cx = chroma == 0 ? 1 : (chroma == 3 ? 1 : 2);
		uint32_t cy = (chroma == 1 ? 2 : 1) * (2 - frameMbsOnly);
		w -= cx * (l + rr);
		h -= cy * (t + bo);
	}
	mWidth = int(w);
	mHeight = int(h);
	return b.ok() && w > 0 && h > 0 && w < 16384 && h < 16384;
}

std::vector<uint8_t> Fmp4Muxer::configRecord() const {
	Buf b;
	if (mHevc) {
		size_t box = b.begin("hvcC");
		b.u8(1);
		b.u8(uint8_t(mProfileSpace << 6 | mTier << 5 | mProfileIdc));
		b.u32(mCompatFlags);
		b.bytes(mConstraint, 6);
		b.u8(mLevelIdc);
		b.u16(0xf000); // min_spatial_segmentation_idc = 0
		b.u8(0xfc);    // parallelismType = 0
		b.u8(uint8_t(0xfc | mChromaFormat));
		b.u8(uint8_t(0xf8 | (mBitDepthLuma - 8)));
		b.u8(uint8_t(0xf8 | (mBitDepthChroma - 8)));
		b.u16(0); // avgFrameRate
		b.u8(uint8_t((mMaxSubLayers & 7) << 3 | (mTemporalIdNested & 1) << 2 | 3)); // lengthSizeMinusOne = 3
		b.u8(3);
		const std::vector<uint8_t> *arrays[3] = {&mVps, &mSps, &mPps};
		const uint8_t types[3] = {32, 33, 34};
		for (int i = 0; i < 3; i++) {
			b.u8(uint8_t(0x80 | types[i])); // array_completeness = 1
			b.u16(1);
			b.u16(uint16_t(arrays[i]->size()));
			b.bytes(*arrays[i]);
		}
		b.end(box);
	} else {
		size_t box = b.begin("avcC");
		b.u8(1);
		b.u8(mProfileIdc);
		b.u8(mAvcConstraint);
		b.u8(mLevelIdc);
		b.u8(0xff); // lengthSizeMinusOne = 3
		b.u8(0xe1); // one SPS
		b.u16(uint16_t(mSps.size()));
		b.bytes(mSps);
		b.u8(1);
		b.u16(uint16_t(mPps.size()));
		b.bytes(mPps);
		b.end(box);
	}
	return b.v;
}

std::string Fmp4Muxer::mime() const {
	char buf[128];
	if (!mHevc) {
		snprintf(buf, sizeof(buf), "video/mp4; codecs=\"avc1.%02X%02X%02X\"", mProfileIdc, mAvcConstraint, mLevelIdc);
		return buf;
	}
	uint32_t rev = 0;
	for (int i = 0; i < 32; i++)
		if (mCompatFlags & (1u << i))
			rev |= 1u << (31 - i);
	std::string s = "hvc1.";
	if (mProfileSpace)
		s += char('A' + mProfileSpace - 1);
	s += std::to_string(mProfileIdc);
	snprintf(buf, sizeof(buf), ".%X.%c%u", rev, mTier ? 'H' : 'L', mLevelIdc);
	s += buf;
	int last = 5;
	while (last >= 0 && mConstraint[last] == 0)
		last--;
	for (int i = 0; i <= last; i++) {
		snprintf(buf, sizeof(buf), ".%X", mConstraint[i]);
		s += buf;
	}
	return "video/mp4; codecs=\"" + s + "\"";
}

std::vector<uint8_t> Fmp4Muxer::initSegment() const {
	Buf b;
	size_t ftyp = b.begin("ftyp");
	b.fourcc("isom");
	b.u32(0x200);
	b.fourcc("isom");
	b.fourcc("iso6");
	b.fourcc("mp41");
	b.end(ftyp);

	size_t moov = b.begin("moov");
	size_t mvhd = b.full("mvhd", 0, 0);
	b.u32(0);
	b.u32(0);
	b.u32(1000);
	b.u32(0);
	b.u32(0x00010000);
	b.u16(0x0100);
	b.zeros(10);
	matrix(b);
	b.zeros(24);
	b.u32(2);
	b.end(mvhd);

	size_t trak = b.begin("trak");
	size_t tkhd = b.full("tkhd", 0, 0x000003);
	b.u32(0);
	b.u32(0);
	b.u32(1);
	b.u32(0);
	b.u32(0);
	b.zeros(8);
	b.u16(0);
	b.u16(0);
	b.u16(0);
	b.u16(0);
	matrix(b);
	b.u32(uint32_t(mWidth) << 16);
	b.u32(uint32_t(mHeight) << 16);
	b.end(tkhd);

	size_t mdia = b.begin("mdia");
	size_t mdhd = b.full("mdhd", 0, 0);
	b.u32(0);
	b.u32(0);
	b.u32(90000);
	b.u32(0);
	b.u16(0x55c4); // "und"
	b.u16(0);
	b.end(mdhd);
	size_t hdlr = b.full("hdlr", 0, 0);
	b.u32(0);
	b.fourcc("vide");
	b.zeros(12);
	const char name[] = "DuoPlay video";
	b.bytes(reinterpret_cast<const uint8_t *>(name), sizeof(name));
	b.end(hdlr);

	size_t minf = b.begin("minf");
	size_t vmhd = b.full("vmhd", 0, 1);
	b.zeros(8);
	b.end(vmhd);
	size_t dinf = b.begin("dinf");
	size_t dref = b.full("dref", 0, 0);
	b.u32(1);
	size_t url = b.full("url ", 0, 1);
	b.end(url);
	b.end(dref);
	b.end(dinf);

	size_t stbl = b.begin("stbl");
	size_t stsd = b.full("stsd", 0, 0);
	b.u32(1);
	size_t entry = b.begin(mHevc ? "hvc1" : "avc1");
	b.zeros(6);
	b.u16(1); // data_reference_index
	b.zeros(16);
	b.u16(uint16_t(mWidth));
	b.u16(uint16_t(mHeight));
	b.u32(0x00480000);
	b.u32(0x00480000);
	b.u32(0);
	b.u16(1);
	b.zeros(32); // compressorname
	b.u16(0x0018);
	b.u16(0xffff);
	b.bytes(configRecord());
	if (mColor.primaries) { // e.g. BT.2020 + PQ for HDR10
		size_t colr = b.begin("colr");
		b.fourcc("nclx");
		b.u16(mColor.primaries);
		b.u16(mColor.transfer);
		b.u16(mColor.matrix);
		b.u8(mColor.fullRange ? 0x80 : 0);
		b.end(colr);
	}
	b.end(entry);
	b.end(stsd);
	for (const char *t : {"stts", "stsc", "stco"}) {
		size_t x = b.full(t, 0, 0);
		b.u32(0);
		b.end(x);
	}
	size_t stsz = b.full("stsz", 0, 0);
	b.u32(0);
	b.u32(0);
	b.end(stsz);
	b.end(stbl);
	b.end(minf);
	b.end(mdia);
	b.end(trak);

	size_t mvex = b.begin("mvex");
	size_t trex = b.full("trex", 0, 0);
	b.u32(1);
	b.u32(1);
	b.u32(0);
	b.u32(0);
	b.u32(0);
	b.end(trex);
	b.end(mvex);
	b.end(moov);
	return b.v;
}

std::vector<uint8_t> Fmp4Muxer::fragment(const uint8_t *au, size_t size, bool keyframe) {
	// Annex-B -> 4-byte length prefixes; parameter sets live in the init segment, AUDs go.
	Buf sample;
	for (const auto &nal : splitNals(au, size, mHevc)) {
		bool drop = mHevc ? (nal.type == 32 || nal.type == 33 || nal.type == 34 || nal.type == 35)
		                  : (nal.type == 7 || nal.type == 8 || nal.type == 9);
		if (drop)
			continue;
		sample.u32(uint32_t(nal.size));
		sample.bytes(nal.data, nal.size);
	}
	uint32_t duration = uint32_t(90000 / mFps);

	Buf b;
	size_t moof = b.begin("moof");
	size_t mfhd = b.full("mfhd", 0, 0);
	b.u32(++mSequence);
	b.end(mfhd);
	size_t traf = b.begin("traf");
	size_t tfhd = b.full("tfhd", 0, 0x020000); // default-base-is-moof
	b.u32(1);
	b.end(tfhd);
	size_t tfdt = b.full("tfdt", 1, 0);
	b.u64(mDecodeTime);
	b.end(tfdt);
	size_t trun = b.full("trun", 0, 0x000001 | 0x000100 | 0x000200 | 0x000400);
	b.u32(1);
	size_t dataOffsetAt = b.v.size();
	b.u32(0);
	b.u32(duration);
	b.u32(uint32_t(sample.v.size()));
	b.u32(keyframe ? 0x02000000 : 0x01010000);
	b.end(trun);
	b.end(traf);
	b.end(moof);
	uint32_t dataOffset = uint32_t(b.v.size() - moof + 8);
	b.v[dataOffsetAt] = uint8_t(dataOffset >> 24);
	b.v[dataOffsetAt + 1] = uint8_t(dataOffset >> 16);
	b.v[dataOffsetAt + 2] = uint8_t(dataOffset >> 8);
	b.v[dataOffsetAt + 3] = uint8_t(dataOffset);
	size_t mdat = b.begin("mdat");
	b.bytes(sample.v);
	b.end(mdat);
	mDecodeTime += duration;
	return b.v;
}

} // namespace duo
