// Fuzzes bimg's container parsers (DDS/KTX/KTX2/PVR3/ASTC) and the pixel-format decoders reached by
// converting every mip of the parsed image to an uncompressed format. The last input byte selects
// the destination format and is not part of the image.
#include <bimg/bimg.h>
#include <bx/allocator.h>
#include <stdint.h>

static const bimg::TextureFormat::Enum kDst[] =
{
	bimg::TextureFormat::RGBA8,
	bimg::TextureFormat::BGRA8,
	bimg::TextureFormat::RGBA32F,
	bimg::TextureFormat::RGBA16F,
	bimg::TextureFormat::R8,
	bimg::TextureFormat::RGB10A2,
};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* _data, size_t _size)
{
	if (_size < 2 || _size > 1 << 20)
	{
		return -1;
	}

	const bimg::TextureFormat::Enum dstFormat = kDst[_data[_size - 1] % BX_COUNTOF(kDst)];
	_size -= 1;

	bimg::ImageContainer ic;
	if (!bimg::imageParse(ic, _data, uint32_t(_size) ) )
	{
		return -1;
	}

	const uint64_t pixels = uint64_t(ic.m_width) * ic.m_height * bx::max<uint32_t>(1, ic.m_depth)
		* bx::max<uint32_t>(1, ic.m_numLayers) * (ic.m_cubeMap ? 6 : 1);
	if (pixels > (1u << 18) )
	{
		return -1;
	}

	bx::DefaultAllocator allocator;
	bimg::ImageContainer* out = bimg::imageConvert(&allocator, dstFormat, ic, true);
	if (NULL != out)
	{
		bimg::imageFree(out);
	}

	return 0;
}
