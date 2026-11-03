// Fuzzes shaderc's self-contained C preprocessor (tools/shaderc/pp.cpp).
// Input layout: optional "-D" block, then source. If the input contains a line "//--", everything
// before it is predefined macros (one per line, `NAME`, `NAME=VALUE` or `NAME(a,b)=body`).
#include "../tools/shaderc/pp.h"

#include <bx/allocator.h>
#include <bx/readerwriter.h>
#include <stdint.h>

namespace
{
	struct Callback : shaderc::PreprocessorCallbackI
	{
		explicit Callback(const bx::StringView& _self) : m_self(_self) {}

		virtual bool include(const bx::StringView& _name, bool, const bx::StringView&, bx::WriterI* _writer, bx::FilePath& _outPath, bx::Error* _err) override
		{
			// Names starting with "self" re-include the input (exercises nesting/#pragma once);
			// everything else resolves to a small header.
			// A self-including input fans out exponentially up to the include-depth limit; cap the total.
			if (++m_includes > 16)
			{
				return false;
			}

			_outPath.set(_name);
			const char hdr[] = "#pragma once\n#define FROM_HDR 1\n#ifndef HDR_FN\n#define HDR_FN(a, b) ((a) + (b))\n#endif\n";
			if (_name.getLength() >= 4 && 0 == bx::strCmp(_name, "self", 4) )
			{
				bx::write(_writer, m_self.getPtr(), m_self.getLength(), _err);
				return _err->isOk();
			}
			bx::write(_writer, hdr, sizeof(hdr) - 1, _err);
			return _err->isOk();
		}

		virtual void depend(const bx::StringView&) override {}
		virtual void message(bool, const shaderc::SourceLocation&, const bx::StringView&) override {}

		bx::StringView m_self;
		uint32_t       m_includes = 0;
	};
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* _data, size_t _size)
{
	if (_size > 1 << 20)
	{
		return -1;
	}

	bx::StringView input( (const char*)_data, int32_t(_size) );
	bx::StringView defines;
	bx::StringView source = input;

	const bx::StringView sep = bx::strFind(input, "\n//--\n");
	if (!sep.isEmpty() )
	{
		defines = bx::StringView(input.getPtr(), sep.getPtr() );
		source  = bx::StringView(sep.getTerm(), input.getTerm() );
	}

	bx::DefaultAllocator allocator;
	Callback callback(source);
	shaderc::Preprocessor pp(callback, &allocator);

	for (const char* ptr = defines.getPtr(); ptr < defines.getTerm(); )
	{
		const char* eol = ptr;
		while (eol < defines.getTerm() && '\n' != *eol)
		{
			++eol;
		}

		const bx::StringView line = bx::strTrim(bx::StringView(ptr, eol), " \t\r\n");
		if (!line.isEmpty() )
		{
			pp.define(line);
		}

		ptr = eol + 1;
	}

	pp.setEmitLineDirectives(0 != (_size & 1) );

	bx::MemoryBlock block(&allocator);
	bx::MemoryWriter writer(&block);
	bx::Error err;
	pp.preprocess("fuzz.sc", source, &writer, &err);

	return 0;
}
