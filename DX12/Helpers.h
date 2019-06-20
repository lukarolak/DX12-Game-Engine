///Contains helper functions

#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

///Throw on fail code
inline void ThrowIfFailed(HRESULT hr)
{
	if (FAILED(hr))
	{
		throw std::exception();
	}
}