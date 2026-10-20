// apx/headless.cpp -- see headless.hpp.
#include "headless.hpp"
#include "../main.hpp"

#include <cstdlib>

namespace apx
{
	bool headlessOn = false;
	bool noInputGrab = false;
	static int batch = 50;

	void headlessInit(bool flag)
	{
		const char* e = getenv("APX_HEADLESS");
		headlessOn = flag || (e && e[0] == '1');
		if ( const char* b = getenv("APX_HEADLESS_BATCH") )
		{
			const int v = atoi(b);
			if ( v >= 1 && v <= 10000 ) { batch = v; }
		}
	}

	int headlessBatch() { return batch; }

	void headlessLog()
	{
		if ( headlessOn )
		{
			printlog("[apx] headless: hidden window, no audio, no vsync, no frame limiter, no drawing, %d logic ticks per pass", batch);
		}
	}
}
