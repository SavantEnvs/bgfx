// Fuzzes bimg's full decode entry point: sniffs and decodes PNG/JPEG/BMP/GIF/TGA/PSD/HDR/PIC/PNM/EXR/
// WebP/AVIF/SVG as well as the texture containers, then converts the result.
#include <bimg/decode.h>
#include <bx/allocator.h>
#include <bx/error.h>
#include <stdint.h>

static const uint64_t kMaxPixels = 1u << 18;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* _data, size_t _size)
{
	if (_size < 4 || _size > 1 << 20)
	{
		return -1;
	}

	bx::DefaultAllocator allocator;
	bx::Error err;

	bimg::ImageContainer info;
	if (bimg::imageParseInfo(&allocator, info, _data, uint32_t(_size), &err)
	&&  uint64_t(info.m_width) * info.m_height * bx::max<uint32_t>(1, info.m_depth) > kMaxPixels)
	{
		return -1;
	}

	err.reset();
	bimg::ImageContainer* ic = bimg::imageParse(&allocator, _data, uint32_t(_size), bimg::TextureFormat::Count, &err);
	if (NULL == ic)
	{
		return -1;
	}

	const uint64_t pixels = uint64_t(ic->m_width) * ic->m_height * bx::max<uint32_t>(1, ic->m_depth)
		* bx::max<uint32_t>(1, ic->m_numLayers) * (ic->m_cubeMap ? 6 : 1);
	if (pixels <= kMaxPixels)
	{
		bimg::ImageContainer* out = bimg::imageConvert(&allocator, bimg::TextureFormat::RGBA8, *ic, false);
		if (NULL != out)
		{
			bimg::imageFree(out);
		}
	}

	bimg::imageFree(ic);
	return 0;
}
