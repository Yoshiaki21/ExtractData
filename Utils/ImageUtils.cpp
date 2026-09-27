#include "StdAfx.h"
#include "Utils/ImageUtils.h"

namespace ImageUtils
{

bool DecodePng(const u8* data, size_t size, u32* width, u32* height, std::vector<u8>* bgra)
{
	// libpng's simplified API; CPng can only write PNG files
	png_image image{};
	image.version = PNG_IMAGE_VERSION;

	if (!png_image_begin_read_from_memory(&image, data, size))
		return false;

	image.format = PNG_FORMAT_BGRA;
	bgra->resize(PNG_IMAGE_SIZE(image));

	if (!png_image_finish_read(&image, nullptr, bgra->data(), 0, nullptr))
	{
		png_image_free(&image);
		return false;
	}

	*width = image.width;
	*height = image.height;
	return true;
}

void BlendOver(std::vector<u8>& dst, u32 dst_width, const std::vector<u8>& src, u32 src_width, u32 src_height, u32 x, u32 y)
{
	for (u32 row = 0; row < src_height; row++)
	{
		const u8* s = &src[static_cast<size_t>(row) * src_width * 4];
		u8* d = &dst[(static_cast<size_t>(y + row) * dst_width + x) * 4];

		for (u32 col = 0; col < src_width; col++, s += 4, d += 4)
		{
			const u32 src_alpha = s[3];
			const u32 dst_alpha = d[3];

			if (src_alpha == 0)
				continue;

			if (src_alpha == 255 || dst_alpha == 0)
			{
				std::memcpy(d, s, 4);
				continue;
			}

			// Weights scaled by 255 * 255
			const u32 src_weight = src_alpha * 255;
			const u32 dst_weight = dst_alpha * (255 - src_alpha);
			const u32 out_weight = src_weight + dst_weight;

			for (int c = 0; c < 3; c++)
				d[c] = static_cast<u8>((s[c] * src_weight + d[c] * dst_weight + out_weight / 2) / out_weight);

			d[3] = static_cast<u8>((out_weight + 127) / 255);
		}
	}
}

} // namespace ImageUtils
