// Known-answer selftest for the code the fuzz targets exercise: bx checksums/string formatting,
// shaderc's preprocessor, and bimg's PNG/TGA decoders and pixel-format conversion.
#include "../tools/shaderc/pp.h"

#include <bimg/decode.h>
#include <bx/allocator.h>
#include <bx/hash.h>
#include <bx/string.h>
#include <bx/readerwriter.h>

#include <stdio.h>
#include <string.h>

static int s_pass = 0;
static int s_fail = 0;

#define CHECK(_cond) \
	do { \
		if (_cond) { ++s_pass; } \
		else { ++s_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #_cond); } \
	} while (0)

namespace
{
	struct Callback : shaderc::PreprocessorCallbackI
	{
		virtual bool include(const bx::StringView& _name, bool, const bx::StringView&, bx::WriterI* _writer, bx::FilePath& _outPath, bx::Error* _err) override
		{
			_outPath.set(_name);
			const char hdr[] = "#define FROM_HDR 7\n";
			bx::write(_writer, hdr, sizeof(hdr) - 1, _err);
			return true;
		}
		virtual void depend(const bx::StringView&) override {}
		virtual void message(bool, const shaderc::SourceLocation&, const bx::StringView&) override {}
	};

	bool preprocess(const char* _source, char* _out, size_t _outSize, const char* _define = NULL)
	{
		bx::DefaultAllocator allocator;
		Callback callback;
		shaderc::Preprocessor pp(callback, &allocator);
		if (NULL != _define)
		{
			pp.define(_define);
		}
		bx::MemoryBlock block(&allocator);
		bx::MemoryWriter writer(&block);
		bx::Error err;
		const bool ok = pp.preprocess("kat.sc", _source, &writer, &err);
		const size_t len = bx::min<size_t>(_outSize - 1, size_t(bx::getSize(&writer) ) );
		memcpy(_out, block.more(0), len);
		_out[len] = '\0';
		return ok && err.isOk();
	}

	const uint8_t kPng2x2[] =
	{
		0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x02,0x08,0x02,0x00,0x00,0x00,0xfd,0xd4,0x9a,
		0x73,0x00,0x00,0x00,0x13,0x49,0x44,0x41,0x54,0x78,0xda,0x63,0xf8,0xcf,0xc0,0xc0,0x00,0xc2,0x0c,0xff,0xb9,0x44,0xe4,0x00,0x1a,0x58,0x03,0x3a,0xe2,0x92,0x6e,
		0xd9,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82,
	};

	// 2x1 uncompressed 24-bit TGA, top-left origin: pixels (R,G,B) = (1,2,3), (4,5,6).
	const uint8_t kTga2x1[] =
	{
		0,0,2, 0,0,0,0,0, 0,0,0,0, 2,0, 1,0, 24, 0x20,
		3,2,1, 6,5,4,
	};

	bool rgba8(const bimg::ImageContainer& _ic, uint8_t _out[16], uint32_t _numPixels)
	{
		bimg::ImageContainer* out = bimg::imageConvert(_ic.m_allocator, bimg::TextureFormat::RGBA8, _ic, false);
		if (NULL == out)
		{
			return false;
		}
		bimg::ImageMip mip;
		const bool ok = bimg::imageGetRawData(*out, 0, 0, out->m_data, out->m_size, mip);
		if (ok)
		{
			memcpy(_out, mip.m_data, _numPixels * 4);
		}
		bimg::imageFree(out);
		return ok;
	}
}

int main()
{
	// bx: checksums with published test vectors, and formatted output.
	{
		bx::HashCrc32 crc;
		crc.begin(bx::HashCrc32::Ieee);
		crc.add("123456789");
		CHECK(0xcbf43926 == crc.end() );

		bx::HashAdler32 adler;
		adler.begin();
		adler.add("Wikipedia");
		CHECK(0x11e60398 == adler.end() );

		char buf[64];
		bx::snprintf(buf, sizeof(buf), "%d-%s-%x", 42, "ok", 255);
		CHECK(0 == strcmp(buf, "42-ok-ff") );
	}

	// shaderc preprocessor.
	{
		char out[512];
		CHECK(preprocess("#define X 3\nint a = X;\n", out, sizeof(out) ) );
		CHECK(NULL != strstr(out, "int a = 3;") );

		CHECK(preprocess("#define ADD(a, b) ((a) + (b))\nint c = ADD(1, 2);\n", out, sizeof(out) ) );
		CHECK(NULL != strstr(out, "int c = ((1) + (2));") );

		CHECK(preprocess("#ifdef FLAG\nyes\n#else\nno\n#endif\n", out, sizeof(out), "FLAG=1") );
		CHECK(NULL != strstr(out, "yes") && NULL == strstr(out, "no") );

		CHECK(preprocess("#ifdef FLAG\nyes\n#else\nno\n#endif\n", out, sizeof(out) ) );
		CHECK(NULL != strstr(out, "no") && NULL == strstr(out, "yes") );

		CHECK(preprocess("#include \"h.sh\"\nint d = FROM_HDR;\n", out, sizeof(out) ) );
		CHECK(NULL != strstr(out, "int d = 7;") );

		CHECK(!preprocess("#error stop\n", out, sizeof(out) ) );
	}

	// bimg decoders and conversion.
	{
		bx::DefaultAllocator allocator;
		bx::Error err;

		bimg::ImageContainer* png = bimg::imageParse(&allocator, kPng2x2, sizeof(kPng2x2), bimg::TextureFormat::Count, &err);
		CHECK(NULL != png);
		if (NULL != png)
		{
			CHECK(2 == png->m_width && 2 == png->m_height);
			uint8_t px[16];
			CHECK(rgba8(*png, px, 4) );
			const uint8_t expect[16] = { 255,0,0,255, 0,255,0,255, 0,0,255,255, 10,20,30,255 };
			CHECK(0 == memcmp(px, expect, sizeof(expect) ) );
			bimg::imageFree(png);
		}

		err.reset();
		bimg::ImageContainer* tga = bimg::imageParse(&allocator, kTga2x1, sizeof(kTga2x1), bimg::TextureFormat::Count, &err);
		CHECK(NULL != tga);
		if (NULL != tga)
		{
			CHECK(2 == tga->m_width && 1 == tga->m_height);
			uint8_t px[8];
			CHECK(rgba8(*tga, px, 2) );
			const uint8_t expect[8] = { 1,2,3,255, 4,5,6,255 };
			CHECK(0 == memcmp(px, expect, sizeof(expect) ) );
			bimg::imageFree(tga);
		}

		// Corrupt data is rejected rather than "decoded".
		err.reset();
		uint8_t bad[sizeof(kPng2x2)];
		memcpy(bad, kPng2x2, sizeof(bad) );
		bad[20] = 0x7f; // absurd width
		bimg::ImageContainer* none = bimg::imageParse(&allocator, bad, sizeof(bad), bimg::TextureFormat::Count, &err);
		CHECK(NULL == none);
		if (NULL != none)
		{
			bimg::imageFree(none);
		}

		// RGBA8 -> BGRA8 swaps channels.
		const uint8_t src[4] = { 1, 2, 3, 4 };
		uint8_t dst[4] = {};
		CHECK(bimg::imageConvert(&allocator, dst, bimg::TextureFormat::BGRA8, src, bimg::TextureFormat::RGBA8, 1, 1, 1) );
		CHECK(3 == dst[0] && 2 == dst[1] && 1 == dst[2] && 4 == dst[3]);
	}

	printf("selftest: %d passed, %d failed\n", s_pass, s_fail);
	return 0 == s_fail ? 0 : 1;
}
