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

//query for a compatible adapter
ComPtr<IDXGIAdapter4> GetAdapter(bool useWarp)
{
	ComPtr<IDXGIFactory4> dxgiFactory;
	UINT createFactoryFlags = 0;
#if defined(_DEBUG)
	createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
	ThrowIfFailed(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory)));
	ComPtr<IDXGIAdapter1> dxgiAdapter1;
	ComPtr<IDXGIAdapter4> dxgiAdapter4;
	if (useWarp)
	{
		//Trebam pogledati https://docs.microsoft.com/en-us/cpp/atl/queryinterface?view=vs-2019
		//Kreira WARP adapter
		ThrowIfFailed(dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&dxgiAdapter1)));
		//EnumWarpAdapter prima IDXGIAdapter1 adapter, no zelimo vratiti IDXGIAdapter4 adapter iz ove funkcije
		//stoga se ovako castaju COM objekti
		ThrowIfFailed(dxgiAdapter1.As(&dxgiAdapter4));
	}
	else
	{
		SIZE_T maxDedicatedVideoMemory = 0;
		//Prodi kroz sve adaptere
		for (UINT i = 0; dxgiFactory->EnumAdapters1(i, &dxgiAdapter1) != DXGI_ERROR_NOT_FOUND; i++)
		{
			DXGI_ADAPTER_DESC1 dxgiAdapterDesc1;
			dxgiAdapter1->GetDesc1(&dxgiAdapterDesc1);

			// Check to see if the adapter can create a D3D12 device without actually 
			// creating it. The adapter with the largest dedicated video memory
			// is favored.
			if ((dxgiAdapterDesc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
			{
				//Kako bi bili sigurni da adapter podrzava dx12 kreiramo dx12 device na njemu
				if (SUCCEEDED(D3D12CreateDevice(dxgiAdapter1.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
				{
					//Zelimo koristiti adapter s najvise video memorije
					if (dxgiAdapterDesc1.DedicatedVideoMemory > maxDedicatedVideoMemory)
					{
						maxDedicatedVideoMemory = dxgiAdapterDesc1.DedicatedVideoMemory;
						//Cast-am adapter u dxgiadapter4, kako bih ga mogao vratiti iz funkcije
						ThrowIfFailed(dxgiAdapter1.As(&dxgiAdapter4));
					}
				}
			}
		}
	}
	return dxgiAdapter4;
}

ComPtr<ID3D12Device> CreateDevice(ComPtr<IDXGIAdapter4> adapter)
{
	ComPtr<ID3D12Device2> d3d12Device2;
	//Create device args
	/*
	* _In_opt_  IUnknown* pAdapter				- pointer na video adapter, mogu poslati NULL, onda koristim prvi adapter kojeg
												//bih dobio iz IDXGIFactory1::EnumAdapters
	* D3D_FEATURE_LEVEL MinimumFeatureLevel
	* _In_ REFIID riid							//GUID for device interface, unique for every device, taj prametar i ppDevice
												//dobivam iz jednog macro-a IID_PPV_ARGS
	* _Out_opt_ void **ppDevice
	*/
	ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12Device2)));
	
	/// Enable debug messages in debug mode.
#if defined(_DEBUG)
	ComPtr<ID3D12InfoQueue> pInfoQueue;
	if (SUCCEEDED(d3d12Device2.As(&pInfoQueue)))
	{
		//Breakaj u visual-u ako dode do error-a ove razine
		pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
		pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
		pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);
		// Suppress whole categories of messages
		//D3D12_MESSAGE_CATEGORY Categories[] = {};

		// Suppress messages based on their severity level
		//Ovo mice poruke samo informativne naravi
		D3D12_MESSAGE_SEVERITY Severities[] = { D3D12_MESSAGE_SEVERITY_INFO };

		// Suppress individual messages by their ID
		D3D12_MESSAGE_ID DenyIds[] = 
		{
			D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,   // I'm really not sure how to avoid this message.
																			/*
																			This warning occurs when a render target is cleared using a clear color
																			that is not the optimized clear color specified during resource creation.
																			If you want to clear a render target using an arbitrary clear color, you should disable this warning.
																			*/
			D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,                         // This warning occurs when using capture frame while graphics debugging.
			D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE,                       // This warning occurs when using capture frame while graphics debugging.
		};

		D3D12_INFO_QUEUE_FILTER NewFilter = {};
		//NewFilter.DenyList.NumCategories = _countof(Categories);
		//NewFilter.DenyList.pCategoryList = Categories;
		NewFilter.DenyList.NumSeverities = _countof(Severities);
		NewFilter.DenyList.pSeverityList = Severities;
		NewFilter.DenyList.NumIDs = _countof(DenyIds);
		NewFilter.DenyList.pIDList = DenyIds;
		//Davanje info queue-u filter
		ThrowIfFailed(pInfoQueue->PushStorageFilter(&NewFilter));
	}
#endif

	return d3d12Device2;
}

ComPtr<ID3D12CommandQueue> CreateCommandQueue(ComPtr<ID3D12Device2> device, D3D12_COMMAND_LIST_TYPE type)
{
	ComPtr<ID3D12CommandQueue> d3d12CommandQueue;
	//D3D12_COMMAND_QUEUE_DESC struktura se sastoji od
	/*
	* D3D12_COMMAND_LIST_TYPE   Type; -Vrsta queue-a mogu biti
									//D3D12_COMMAND_LIST_TYPE_DIRECT -draw, compute, copy commands
									//D3D12_COMMAND_LIST_TYPE_COMPUTE -compute, copy commands
									//D3D12_COMMAND_LIST_TYPE_COPY -copy commands
	* INT                       Priority; - prioritet
									//D3D12_COMMAND_QUEUE_PRIORITY_NORMAL
									//D3D12_COMMAND_QUEUE_PRIORITY_HIGH
									//D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME
	* D3D12_COMMAND_QUEUE_FLAGS Flags;
	* UINT                      NodeMask; //U slucaju vise GPU node-ova, ova maska se postavlja https://docs.microsoft.com/en-us/windows/desktop/direct3d12/mulit-engine
	*/
	D3D12_COMMAND_QUEUE_DESC desc = {};
	desc.Type = type;
	desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	desc.NodeMask = 0;

	//Kreiranje command queue-a
	ThrowIfFailed(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&d3d12CommandQueue)));

	return d3d12CommandQueue;
}

bool CheckTearingSupport()
{
	BOOL allowTearing = FALSE;

	// Rather than create the DXGI 1.5 factory interface directly, we create the
	// DXGI 1.4 interface and query for the 1.5 interface. This is to enable the 
	// graphics debugging tools which will not support the 1.5 factory interface 
	// until a future update.
	ComPtr<IDXGIFactory4> factory4;
	if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory4))))
	{
		ComPtr<IDXGIFactory5> factory5;
		if (SUCCEEDED(factory4.As(&factory5)))
		{
			//CheckFeatureSupport args:
			/*
			* DXGI_FEATURE		Feature -enum samo evidentno je koje opcije ima
			* [in, out] void    *pFeatureSupportData -pointer na data-u koja ce biti ispunjena s informacijama
			* UINT				FeatureSupportDataSize
			*
			*/
			if (FAILED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
			{
				allowTearing = FALSE;
			}
		}
	}

	return allowTearing == TRUE;
}

ComPtr<IDXGISwapChain4> CreateSwapChain(HWND hWnd,ComPtr<ID3D12CommandQueue> commandQueue,uint32_t width, uint32_t height, uint32_t bufferCount)
{
	ComPtr<IDXGISwapChain4> dxgiSwapChain4;
	ComPtr<IDXGIFactory4> dxgiFactory4;
	UINT createFactoryFlags = 0;
#if defined(_DEBUG)
	createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
	//Kreiranje factory-a
	ThrowIfFailed(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory4)));

	//DXGI_SWAP_CHAIN_DESC1 args:
	/*
	* UINT             Width; -Resolution width
	* UINT             Height; -Resolution height
	* DXGI_FORMAT      Format; -Display format
	* BOOL             Stereo; -No idea
	* DXGI_SAMPLE_DESC SampleDesc; -Struktura koja opisuje multi-sampling parametre, ovaj member je
									//validan jedino ako se koriste bitblit model swap chain-ovi
									//a mislim da u dx12 toga nema. Kada se koristi flip model swap chain
									//ovaj member mora biti {1, 0}
	* DXGI_USAGE       BufferUsage; -cemu buffer sluzi, moze biti:
									//DXGI_USAGE_SHADER_INPUT
									//DXGI_USAGE_RENDER_TARGET_OUTPUT
	* UINT             BufferCount; -kolicina buffer-a u chain-u
	* DXGI_SCALING     Scaling; //ako velicina back buffer-a nije jednaka target output-u, kako ce se skalirati output
	* DXGI_SWAP_EFFECT SwapEffect; //presentation model kojeg koristi swap chain:
									//DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL nakon present sadrzaj back buffer-a ostaje ne promjenjen
									//DXGI_SWAP_EFFECT_FLIP_DISCARD nakon present sadrzaj back buffera-a se brise?
	* DXGI_ALPHA_MODE  AlphaMode; //Kako ce se ponasati transparentnost:
									//DXGI_ALPHA_MODE_UNSPECIFIED
									//DXGI_ALPHA_MODE_PREMULTIPLIED svaka boja se mnozi s alfom
									//DXGI_ALPHA_MODE_STRAIGHT - no idea, treba istraziti
									//DXGI_ALPHA_MODE_IGNORE -ignoriraj transparentnost
	* UINT             Flags; -Kombinacija DXGI_SWAP_CHAIN_FLAG flagova s bitwise or operatorom
							//ako se podrzava tearing support, ovaj flag mora biti ukljucen
							//DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
	*/
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.Width = width;
	swapChainDesc.Height = height;
	swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapChainDesc.Stereo = FALSE;
	swapChainDesc.SampleDesc = { 1, 0 };
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = bufferCount;
	swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
	// It is recommended to always allow tearing if tearing support is available.
	swapChainDesc.Flags = CheckTearingSupport() ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
	ComPtr<IDXGISwapChain1> swapChain1;

	//Kreiranje swap chain-a, CreateSwapChainForHwnd args:
	/*
	* [in]                 IUnknown                        *pDevice, -Pointer do command queue-a
	* [in]                 HWND                            hWnd, -handle kojeg kreira funkcija
	* [in]           const DXGI_SWAP_CHAIN_DESC1           *pDesc, -Pointer do swap chain-a
	* [in, optional] const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc, - pointer do DXGI_SWAP_CHAIN_FULLSCREEN_DESC strukture
																		//koja radi nesto za fullscreen swap chain, ako je null, kreira se windowed
																		// swap chain
	* [in, optional]       IDXGIOutput                     *pRestrictToOutput, // restriktaj output na nekom drugom output-u
						
	* [out]                IDXGISwapChain1                 **ppSwapChain
	*/
	ThrowIfFailed(dxgiFactory4->CreateSwapChainForHwnd(commandQueue.Get(),hWnd,&swapChainDesc,nullptr,nullptr,&swapChain1));

	// Disable the Alt+Enter fullscreen toggle feature. Switching to fullscreen
	// will be handled manually.
	ThrowIfFailed(dxgiFactory4->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER));

	ThrowIfFailed(swapChain1.As(&dxgiSwapChain4));

	return dxgiSwapChain4;
}
