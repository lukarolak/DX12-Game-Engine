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
bool g_Fullscreen = false;

//Window Callback function
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

//Parsiraj command line argumente
void ParseCommandLineArguments()
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

ComPtr<ID3D12Device2> CreateDevice(ComPtr<IDXGIAdapter4> adapter)
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

//Kreiranje descript heap-a, odnosno array resource view-a
//Resource view moze biti:
//Render target view, Shader resource view, unorderd access view, constant buffer view
ComPtr<ID3D12DescriptorHeap> CreateDescriptorHeap(ComPtr<ID3D12Device2> device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t numDescriptors)
{
	ComPtr<ID3D12DescriptorHeap> descriptorHeap;
	//D3D12_DESCRIPTOR_HEAP_DESC args:
	/*
	* D3D12_DESCRIPTOR_HEAP_TYPE  Type;
				//Tip moze biti:
				//D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV descriptor heap za kombinaciju constant buffer, shader resource i unordered acces view-a
				//D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER descriptor heap za sampler?
				//D3D12_DESCRIPTOR_HEAP_TYPE_RTV descriptor heap  za render target view
				//D3D12_DESCRIPTOR_HEAP_TYPE_DSV descriptor heap za depth stencil view
	* UINT                        NumDescriptors;
				//broj descriptora na heap-u
	* D3D12_DESCRIPTOR_HEAP_FLAGS Flags;
	* UINT                        NodeMask;
				//kombinacija D3D12_DESCRIPTOR_HEAP_FLAGS
	*/
	D3D12_DESCRIPTOR_HEAP_DESC desc = {};
	desc.NumDescriptors = numDescriptors;
	desc.Type = type;

	ThrowIfFailed(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&descriptorHeap)));

	return descriptorHeap;
}

//Render target view
//Opisuje resurs koji se moze attach-ati na slot u unutar merger stage-a, render target view
//opisuje resurs koji prima funal color izracunatu od pixel shader stage-a
void UpdateRenderTargetViews(ComPtr<ID3D12Device2> device,ComPtr<IDXGISwapChain4> swapChain, ComPtr<ID3D12DescriptorHeap> descriptorHeap)
{
	//velicina descriptora je vendor specific, stoga ovdje dohvacamo tu vrijednost
	auto rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(descriptorHeap->GetCPUDescriptorHandleForHeapStart());

	for (int i = 0; i < g_NumFrames; ++i)
	{
		ComPtr<ID3D12Resource> backBuffer;
		//Dohvacam svaki back buffer u swap chain-u
		ThrowIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer)));
		//Kreiranje render target view-a
		device->CreateRenderTargetView(backBuffer.Get(), nullptr, rtvHandle);

		g_BackBuffers[i] = backBuffer;
		//Pomakni pointer za velicinu deskriptora
		rtvHandle.Offset(rtvDescriptorSize);
	}
}

//Command allocator
ComPtr<ID3D12CommandAllocator> CreateCommandAllocator(ComPtr<ID3D12Device2> device,D3D12_COMMAND_LIST_TYPE type)
{
	ComPtr<ID3D12CommandAllocator> commandAllocator;
	//D3D12_COMMAND_LIST_TYPE ima ove tipove:
	//D3D12_COMMAND_LIST_TYPE_DIRECT -command buffer GPU can execute, direct buffer ne nasljeduje GPU state?
	//D3D12_COMMAND_LIST_TYPE_BUNDLE - command buffer koji nasljeduje GPU state
	//D3D12_COMMAND_LIST_TYPE_COMPUTE - command buffer za computing
	//D3D12_COMMAND_LIST_TYPE_COPY -command buffer za kopiranje

	ThrowIfFailed(device->CreateCommandAllocator(type, IID_PPV_ARGS(&commandAllocator)));

	return commandAllocator;
}

//Command list
//sadrzi napredpe koje ce biti pokrenute na GPU-u
ComPtr<ID3D12GraphicsCommandList> CreateCommandList(ComPtr<ID3D12Device2> device, ComPtr<ID3D12CommandAllocator> commandAllocator, D3D12_COMMAND_LIST_TYPE type)
{
	ComPtr<ID3D12GraphicsCommandList> commandList;
	//CreateCommandList args
	/*
	* [in]           UINT                    nodeMask,
				//Gpu node-ovi
	* [in]           D3D12_COMMAND_LIST_TYPE type,
				//type objasenjen gore
	* [in]           ID3D12CommandAllocator  *pCommandAllocator,
				//pointer na alokator, alokator  stvara command list?
	* [in, optional] ID3D12PipelineState     *pInitialState
	* REFIID         riid,
	*[out]           void                    **ppCommandList
	*/
	ThrowIfFailed(device->CreateCommandList(0, type, commandAllocator.Get(), nullptr, IID_PPV_ARGS(&commandList)));

	ThrowIfFailed(commandList->Close());

	return commandList;
}

///GPU sinkronizacija
ComPtr<ID3D12Fence> CreateFence(ComPtr<ID3D12Device2> device)
{
	ComPtr<ID3D12Fence> fence;

	ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));

	return fence;
}
//OS event handle, govori CPU thread-u da ceka
HANDLE CreateEventHandle()
{
	HANDLE fenceEvent;

	fenceEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
	assert(fenceEvent && "Failed to create fence event.");

	return fenceEvent;
}
//Signal the fence
uint64_t Signal(ComPtr<ID3D12CommandQueue> commandQueue, ComPtr<ID3D12Fence> fence, uint64_t& fenceValue)
{
	uint64_t fenceValueForSignal = ++fenceValue;
	//setira fence na vrijednost
	ThrowIfFailed(commandQueue->Signal(fence.Get(), fenceValueForSignal));

	return fenceValueForSignal;
}
//cekaj na fence value
void WaitForFenceValue(ComPtr<ID3D12Fence> fence, uint64_t fenceValue, HANDLE fenceEvent,std::chrono::milliseconds duration = std::chrono::milliseconds::max())
{
	if (fence->GetCompletedValue() < fenceValue)
	{
		ThrowIfFailed(fence->SetEventOnCompletion(fenceValue, fenceEvent));
		::WaitForSingleObject(fenceEvent, static_cast<DWORD>(duration.count()));
	}
}
//flush the GPU, osiguraj da sve napredbe pokrenute na GPU-u su zavrsilie
void Flush(ComPtr<ID3D12CommandQueue> commandQueue, ComPtr<ID3D12Fence> fence,uint64_t& fenceValue, HANDLE fenceEvent)
{
	uint64_t fenceValueForSignal = Signal(commandQueue, fence, fenceValue);
	WaitForFenceValue(fence, fenceValueForSignal, fenceEvent);
}

void Update()
{
	static uint64_t frameCounter = 0;
	static double elapsedSeconds = 0.0;
	static std::chrono::high_resolution_clock clock;
	static auto t0 = clock.now();

	frameCounter++;
	auto t1 = clock.now();
	auto deltaTime = t1 - t0;
	t0 = t1;
	elapsedSeconds += deltaTime.count() * 1e-9;
	if (elapsedSeconds > 1.0)
	{
		char buffer[500];
		auto fps = frameCounter / elapsedSeconds;
		sprintf_s(buffer, 500, "FPS: %f\n", fps);
		OutputDebugString(buffer);

		frameCounter = 0;
		elapsedSeconds = 0.0;
	}
}
//Render funkcija, sastoji se od dva dijela, ocisti back buffer->presentaj rendered frame
void Render()
{
	auto commandAllocator = g_CommandAllocators[g_CurrentBackBufferIndex];
	auto backBuffer = g_BackBuffers[g_CurrentBackBufferIndex];

	commandAllocator->Reset();
	g_CommandList->Reset(commandAllocator.Get(), nullptr);
	// Clear the render target.
	{
		//prebaci sve subresurse na isti state
		CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(backBuffer.Get(),D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

		g_CommandList->ResourceBarrier(1, &barrier);

		FLOAT clearColor[] = { 0.4f, 0.6f, 0.9f, 1.0f };
		CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(g_RTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),g_CurrentBackBufferIndex, g_RTVDescriptorSize);
		//Ocisti RTV
		g_CommandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
	}
	// Present
	{
		CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(backBuffer.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
		g_CommandList->ResourceBarrier(1, &barrier);
		ThrowIfFailed(g_CommandList->Close());

		ID3D12CommandList* const commandLists[] = {g_CommandList.Get()};
		//Pokreni naredbe iz command listi
		g_CommandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);

		UINT syncInterval = g_VSync ? 1 : 0;
		UINT presentFlags = g_TearingSupported && !g_VSync ? DXGI_PRESENT_ALLOW_TEARING : 0;
		ThrowIfFailed(g_SwapChain->Present(syncInterval, presentFlags));
		g_FrameFenceValues[g_CurrentBackBufferIndex] = Signal(g_CommandQueue, g_Fence, g_FenceValue);
		g_CurrentBackBufferIndex = g_SwapChain->GetCurrentBackBufferIndex();
		WaitForFenceValue(g_Fence, g_FrameFenceValues[g_CurrentBackBufferIndex], g_FenceEvent);
	}
}
//Resize swap chain buffers
void Resize(uint32_t width, uint32_t height)
{
	if (g_ClientWidth != width || g_ClientHeight != height)
	{
		// Don't allow 0 size swap chain back buffers.
		g_ClientWidth = std::max(1u, width);
		g_ClientHeight = std::max(1u, height);

		// Flush the GPU queue to make sure the swap chain's back buffers
		// are not being referenced by an in-flight command list.
		Flush(g_CommandQueue, g_Fence, g_FenceValue, g_FenceEvent);
		for (int i = 0; i < g_NumFrames; ++i)
		{
			// Any references to the back buffers must be released
			// before the swap chain can be resized.
			g_BackBuffers[i].Reset();
			g_FrameFenceValues[i] = g_FrameFenceValues[g_CurrentBackBufferIndex];
		}
		DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
		ThrowIfFailed(g_SwapChain->GetDesc(&swapChainDesc));
		ThrowIfFailed(g_SwapChain->ResizeBuffers(g_NumFrames, g_ClientWidth, g_ClientHeight, swapChainDesc.BufferDesc.Format, swapChainDesc.Flags));

		g_CurrentBackBufferIndex = g_SwapChain->GetCurrentBackBufferIndex();

		UpdateRenderTargetViews(g_Device, g_SwapChain, g_RTVDescriptorHeap);
	}
}

void SetFullscreen(bool fullscreen)
{
	if (g_Fullscreen != fullscreen)
	{
		g_Fullscreen = fullscreen;

		if (g_Fullscreen) // Switching to fullscreen.
		{
			// Store the current window dimensions so they can be restored 
			// when switching out of fullscreen state.
			::GetWindowRect(g_hWnd, &g_WindowRect);

			// Set the window style to a borderless window so the client area fills
			// the entire screen.
			UINT windowStyle = WS_OVERLAPPEDWINDOW & ~(WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);

			::SetWindowLongW(g_hWnd, GWL_STYLE, windowStyle);

			// Query the name of the nearest display device for the window.
			// This is required to set the fullscreen dimensions of the window
			// when using a multi-monitor setup.
			HMONITOR hMonitor = ::MonitorFromWindow(g_hWnd, MONITOR_DEFAULTTONEAREST);
			MONITORINFOEX monitorInfo = {};
			monitorInfo.cbSize = sizeof(MONITORINFOEX);
			::GetMonitorInfo(hMonitor, &monitorInfo);

			::SetWindowPos(g_hWnd, HWND_TOP,
				monitorInfo.rcMonitor.left,
				monitorInfo.rcMonitor.top,
				monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
				monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
				SWP_FRAMECHANGED | SWP_NOACTIVATE);

			::ShowWindow(g_hWnd, SW_MAXIMIZE);
		}
		else
		{
			// Restore all the window decorators.
			::SetWindowLong(g_hWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);

			::SetWindowPos(g_hWnd, HWND_NOTOPMOST,
				g_WindowRect.left,
				g_WindowRect.top,
				g_WindowRect.right - g_WindowRect.left,
				g_WindowRect.bottom - g_WindowRect.top,
				SWP_FRAMECHANGED | SWP_NOACTIVATE);

			::ShowWindow(g_hWnd, SW_NORMAL);
		}
	}
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (g_IsInitialized)
	{
		switch (message)
		{
		case WM_PAINT:
			Update();
			Render();
			break;
		case WM_SYSKEYDOWN:
		case WM_KEYDOWN:
		{
			bool alt = (::GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

			switch (wParam)
			{
			case 'V':
				g_VSync = !g_VSync;
				break;
			case VK_ESCAPE:
				::PostQuitMessage(0);
				break;
			case VK_RETURN:
				if (alt)
				{
			case VK_F11:
				SetFullscreen(!g_Fullscreen);
				}
				break;
			}
		}
		break;
		// The default window procedure will play a system notification sound 
		// when pressing the Alt+Enter keyboard combination if this message is 
		// not handled.
		case WM_SYSCHAR:
			break;
		case WM_SIZE:
		{
			RECT clientRect = {};
			::GetClientRect(g_hWnd, &clientRect);

			int width = clientRect.right - clientRect.left;
			int height = clientRect.bottom - clientRect.top;

			Resize(width, height);
		}
		break;
		case WM_DESTROY:
			::PostQuitMessage(0);
			break;
		default:
			return ::DefWindowProcW(hwnd, message, wParam, lParam);
		}
	}
	else
	{
		return ::DefWindowProcW(hwnd, message, wParam, lParam);
	}

	return 0;
}
int CALLBACK wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nCmdShow)
{
	// Windows 10 Creators update adds Per Monitor V2 DPI awareness context.
	// Using this awareness context allows the client area of the window 
	// to achieve 100% scaling while still allowing non-client window content to 
	// be rendered in a DPI sensitive fashion.
	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	// Window class name. Used for registering / creating the window.
	const wchar_t* windowClassName = L"DX12WindowClass";
	ParseCommandLineArguments();
	EnableDebugLayer();

	g_TearingSupported = CheckTearingSupport();

	RegisterWindowClass(hInstance, windowClassName);
	g_hWnd = CreateWindow(windowClassName, hInstance, L"Learning DirectX 12",
		g_ClientWidth, g_ClientHeight);

	// Initialize the global window rect variable.
	::GetWindowRect(g_hWnd, &g_WindowRect);

	ComPtr<IDXGIAdapter4> dxgiAdapter4 = GetAdapter(g_UseWarp);

	g_Device = CreateDevice(dxgiAdapter4);

	g_CommandQueue = CreateCommandQueue(g_Device, D3D12_COMMAND_LIST_TYPE_DIRECT);

	g_SwapChain = CreateSwapChain(g_hWnd, g_CommandQueue,
		g_ClientWidth, g_ClientHeight, g_NumFrames);

	g_CurrentBackBufferIndex = g_SwapChain->GetCurrentBackBufferIndex();

	g_RTVDescriptorHeap = CreateDescriptorHeap(g_Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, g_NumFrames);
	g_RTVDescriptorSize = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	UpdateRenderTargetViews(g_Device, g_SwapChain, g_RTVDescriptorHeap);

	for (int i = 0; i < g_NumFrames; ++i)
	{
		g_CommandAllocators[i] = CreateCommandAllocator(g_Device, D3D12_COMMAND_LIST_TYPE_DIRECT);
	}
	g_CommandList = CreateCommandList(g_Device,
	g_CommandAllocators[g_CurrentBackBufferIndex], D3D12_COMMAND_LIST_TYPE_DIRECT);

	g_Fence = CreateFence(g_Device);
	g_FenceEvent = CreateEventHandle();

	g_IsInitialized = true;

	::ShowWindow(g_hWnd, SW_SHOW);

	MSG msg = {};
	while (msg.message != WM_QUIT)
	{
		if (::PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
		{
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
		}
	}

	// Make sure the command queue has finished all commands before closing.
	Flush(g_CommandQueue, g_Fence, g_FenceValue, g_FenceEvent);

	::CloseHandle(g_FenceEvent);

	return 0;
}


