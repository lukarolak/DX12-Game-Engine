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

//The number of swap chain back buffers, ovo ce kasnije biti objasnjeno
const uint8_t g_NumFrames = 3;

//Use WARP adapter, ako sam dobro shvatio WARP moze provjeriti rendering tehnike ako je kvaliteta drivera upitna
bool g_UseWarp = false;

//Velicina prozora
uint32_t g_ClientWidth = 1280;
uint32_t g_ClientHeight = 720;

//Flag koji se set-a na true kada se DX12 objekti inicializiraju
bool g_IsInitialized = false;

//Window handle
HWND g_hWnd;
//Window rectangle, ovdje se sprema rezolucija prije prelaska u fullscreen mode, kako bi se kasnje vratili
RECT g_WindowRect;

///DirectX 12 objekti
ComPtr<ID3D12Device2> g_Device;
ComPtr<ID3D12CommandQueue> g_CommandQueue;
//The swap chain is responsible for presenting the rendered image to the window, o ovom ce biti vise kasnje
ComPtr<IDXGISwapChain4> g_SwapChain;
//Swap chain gore, kreira back buffer-e, ovdje drzim pointer-e na njih kako bih ih nekada stavio u pravo stanje
//Buffer i texture resusursi se referenciraju preko ID3D12Resource interface-a
ComPtr<ID3D12Resource> g_BackBuffers[g_NumFrames];
//GPU naredbe se spremaju u ovu listu, jedna lista je potrebna unutar jednog thread-a
ComPtr<ID3D12GraphicsCommandList> g_CommandList;
//Predpostavljam da preko command allocatora pisem nove naredbe u command listu, tip prica da ne smijem
//resetirati allocator dok se sve naredbe nisu izvrsile, sto mi je dalo tu ideju
//moram imati jedan alokator prema svakom render frame-u, odnosno jedan po svakom swap chain-u ili back buffer-u
ComPtr<ID3D12CommandAllocator> g_CommandAllocators[g_NumFrames];
//render target view, govori mi gdje je lokacija texture resursa u GPU memoriji, dimenziju teksture i format
//RTV-i su se prije pohranjivali jedan po jedan, sada se pohranjuju u descriptor heap-ove
//Mislim da je jedan RTV po teksutri
ComPtr<ID3D12DescriptorHeap> g_RTVDescriptorHeap;
//Deskriptori unutar heap-a su vendor specific velicine, tome mi sluzi ova varijabla
UINT g_RTVDescriptorSize;
//Indeks trenutnog back buffera ne mora biti sekvencialan, tome mi sluzi ova varijabla
UINT g_CurrentBackBufferIndex;

///Objekti vezani uz GPU sinkrinizaciju
//fence objekt
ComPtr<ID3D12Fence> g_Fence;
//ne znam zasto imamo ovu varijablu i array ispod
uint64_t g_FenceValue = 0;
//za svaki rendered frame koji moze biti "in-flight", moram pratit fence value da bih garantirao da vise ne koristim resurse
//referencirane u command listi, inace bih ih overwrite-ao
uint64_t g_FrameFenceValues[g_NumFrames] = {};
//handle za OS event, koji ce primiti da je fence dostigao specificnu vrijednost
HANDLE g_FenceEvent;

bool g_VSync = true;
bool g_TearingSupported = false;
bool g_fullscreen = false;

//Window Callback function
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

//Parsiraj command line argumente
void ParseCommandLIneArguments()
{
	//::func notacija, je prisutna za funkcije definirane u globalnom scope-u, odnosno system funkcije
	/* Definirane funkcije
	* -w --width		-Width render window-a
	* -h --height		-Height render window-a
	* -warp --warp		-Koristi WARP za device creation
	*/
	int argc;
	wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
	for (size_t i = 0; i < (size_t)argc; i++)
	{
		if (::wcscmp(argv[i], L"-w") == 0 || ::wcscmp(argv[i], L"--width") == 0)
		{
			g_ClientWidth = ::wcstol(argv[++i], nullptr, 10);
		}
		if (::wcscmp(argv[i], L"-h") == 0 || ::wcscmp(argv[i], L"--height") == 0)
		{
			g_ClientHeight = ::wcstol(argv[++i], nullptr, 10);
		}
		if (::wcscmp(argv[i], L"-warp") == 0 || ::wcscmp(argv[i], L"--warp") == 0)
		{
			g_UseWarp = true;
		}
		//Oslobodi memoriju alociranu od strane CommandLineToArgvW funkcije
		::LocalFree(argv);
	}
}

void EnableDebugLayer()
{
#if defined(_DEBUG)
	// Always enable the debug layer before doing anything DX12 related
	// so all possible errors generated while creating DX12 objects
	// are caught by the debug layer.
	ComPtr<ID3D12Debug> debugInterface;
	ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface)));
	debugInterface->EnableDebugLayer();
#endif
}
//Registriraj window class
void RegisterWindowClass(HINSTANCE hInst, const wchar_t* windowClassName)
{
	// Register a window class for creating our render window with.
	WNDCLASSEXW windowClass = {};
	//Velicina u bytovima strukture
	windowClass.cbSize = sizeof(WNDCLASSEX);
	//CS_HREDRAW - redraw-aj window ako se promjeni sirina, CS_VREDRAW - visina
	windowClass.style = CS_HREDRAW | CS_VREDRAW;
	//Pointer do windows procedure koja ce handle-ati window poruke
	windowClass.lpfnWndProc = &WndProc;
	//Broj byte-ova koji se trebaju alocirati nakon WNDCLASSEX strukture
	windowClass.cbClsExtra = 0;
	//Broj byte-ova koji se trebaju alocirati nakon instance prozora
	windowClass.cbWndExtra = 0;
	//Handle do instance koja drzi prozor
	windowClass.hInstance = hInst;
	//Handle to ikone
	windowClass.hIcon = ::LoadIcon(hInst, NULL);
	//Handle to mouse cursora
	windowClass.hCursor = ::LoadCursor(NULL, IDC_ARROW);
	//Pozadina window-a
	windowClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	//Pointer na null terminated string, koji ima resource name od menija klase?
	windowClass.lpszMenuName = NULL;
	//Ime window klase
	windowClass.lpszClassName = windowClassName;
	//Handle na malu ikonu
	windowClass.hIconSm = ::LoadIcon(hInst, NULL);

	static ATOM atom = ::RegisterClassExW(&windowClass);
	assert(atom > 0);
}
 //Kreiraj window
HWND CreateWindow(const wchar_t* windowClassName, HINSTANCE hInst, const wchar_t* windowTitle, uint32_t width, uint32_t height)
{
	//GetSystemMetrics dohvaca system metrics, trenutno visinu i sirinu u pixelima ekrana
	int screenWidth = ::GetSystemMetrics(SM_CXSCREEN);
	int screenHeight = ::GetSystemMetrics(SM_CYSCREEN);

	//dohvati window rect, prema zeljenoj sirini i visini
	RECT windowRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
	::AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

	int windowWidth = windowRect.right - windowRect.left;
	int windowHeight = windowRect.bottom - windowRect.top;

	// Center the window within the screen. Clamp to 0, 0 for the top-left corner.
	int windowX = std::max<int>(0, (screenWidth - windowWidth) / 2);
	int windowY = std::max<int>(0, (screenHeight - windowHeight) / 2);

	//CreateWindowExW parametri:
	/*
	* _In_ DWORD dwExStyle -Extended window style, google-aj za popis
	* _In_opt_ LPCWSTR lpClassName -ime window klase
	* LPCWSTR lpWindowName -window title, treba istraziti
	* DWORD dwStyle, stil prozora, moze biti kombinacija "window style values"
	* int x -Pozicija prozora na ekranu
	* int y -Pozicija prozora na ekranu
	* int nWidth -sirina prozora
	* int nHeight -visina prozora
	* HWND hWndParent -Handle na parent-a ili owner-a prozora
	* HWND hMenu -Handle na meni, treba istraziti?
	* HINSTANCE hInstance - Handle na instancu module koji se povezuje s ovim prozorom
	* LPVOID lpParam -Pointer na vreijednost koja se salje prozoru kroz CREATESTRUCTA strukturu
	*/
	HWND hWnd = ::CreateWindowExW(
		NULL,
		windowClassName,
		windowTitle,
		WS_OVERLAPPEDWINDOW,
		windowX,
		windowY,
		windowWidth,
		windowHeight,
		NULL,
		NULL,
		hInst,
		nullptr
	);

	assert(hWnd && "Failed to create window");

	return hWnd;
}
