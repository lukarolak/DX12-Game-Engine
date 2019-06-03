//Brzi build time, jer ne include-am neke windows.h pod header-e
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
//Command line argumenti -> aplikacija
#include <shellapi.h>

//Stiti me od conflikata od stranje drugih header-a koje implementiraju min/max funkcije
#if defined(min)
#undef min
#endif

#if defined(max)
#undef max
#endif

//Lik koji je napravio tutorial zeli imati funkciju CreateWindow pa je ovdje undef-a
#if defined(CreateWindow)
#undef CreateWindow
#endif

//Needed for Microsoft::WRL::ComPtr<> template class.
#include <wrl.h>

using namespace Microsoft::WRL;

//DirectX 12 specificni header-i
#include <d3d12.h>
//Low level tasks, enumeriranje gpu adaptera, prezentiranje rendera na screen, handle-anje full-screen tranzicija; 1.6 moze detektirati HDR display-eve
#include <dxgi1_6.h>
//kompajliranje HLSL shadera na runtime-u
#include <d3dcompiler.h>
//matematicke funkcije koje se koriste pri grafickom programiranju
#include <DirectXMath.h>

//Ekstenzija za D3D12 library, nije unutar VS-a, mora se skinuti na https://github.com/Microsoft/DirectX-Graphics-Samples/tree/master/Libraries/D3DX12
#include "Library/D3DX12/d3dx12.h"

//Headeri iz Standard Template Library-a
//math funkcije eg. std::min, std::max
#include <algorithm>
//sadrzi asser macro
#include <cassert>
//funkcije vezane s vremenom
#include <chrono>

//Pomocne Funkcije
#include "Helpers.h"


