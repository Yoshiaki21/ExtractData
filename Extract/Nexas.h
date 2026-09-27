#pragma once

#include "ExtractBase.h"

// Giga NeXAS engine .pac archives whose index is stored
// Huffman-compressed at the end of the file.
class CNexas final : public CExtractBase
{
public:
	bool Mount(CArcFile* archive) override;
};
