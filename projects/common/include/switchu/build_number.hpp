#pragma once
// The build number ("build <N>"). CI writes this file before compiling (.github/workflows/switch.yml),
// so only the files that include it are compiled again for a new build: the compile cache keeps the rest.
#ifndef HOMENX_BUILD
#define HOMENX_BUILD "build 0"
#endif
