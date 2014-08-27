#include "stdafx.h"

#include <windows.h>
#include <wininet.h>

#include "htmlctrl.h"

#include "IEBrowserEngine.h"
#include "common/RhoConf.h"
#include "MainWindow.h"

IMPLEMENT_LOGCLASS(CIEBrowserEngine,"IEBrowser");

extern "C" HWND rho_wmimpl_get_mainwnd();
extern "C" LRESULT rho_wm_appmanager_ProcessOnTopMostWnd( WPARAM wParam, LPARAM lParam );
extern "C" void rho_wmimpl_create_ieBrowserEngine(HWND hwndParent, HINSTANCE rhoAppInstance);

//////////////////////////////////////////////////////////////////////////

#define NM_PIE_TITLE            (WM_USER + 104)  ///< Message ID indicating the associated message defines the page title.
#define NM_PIE_META             (WM_USER + 105)  ///< Message ID indicating the associated message defines a Meta Tag (HTTP-Equiv only for WM).
#define NM_PIE_BEFORENAVIGATE   (WM_USER + 109)  ///< Message ID indicating the associated message indicates a BeforeNavigate Event.
#define NM_PIE_DOCUMENTCOMPLETE	(WM_USER + 110)  ///< Message ID indicating the associated message defines a DocumentComplete Event.
#define NM_PIE_NAVIGATECOMPLETE (WM_USER + 111)  ///< Message ID indicating the associated message defines a NavigateComplete Event.
#define NM_PIE_KEYSTATE			(WM_USER + 112)  ///< Message ID indicating the associated message notifies that the key state has changed.
#define NM_PIE_ALPHAKEYSTATE	(WM_USER + 113)  ///< Message ID indicating the associated message notifies that the alpha key state has changed.

#define HTML_CONTAINER_NAME		TEXT("HTMLContainer")  ///< Name of the Window which is parent to all HTML components and handles notifications from the components.

//////////////////////////////////////////////////////////////////////////

HWND              CIEBrowserEngine::g_hwndTabHTMLContainer = NULL;
CIEBrowserEngine* CIEBrowserEngine::g_hInstance = NULL;

//////////////////////////////////////////////////////////////////////////

CIEBrowserEngine* CIEBrowserEngine::getInstance()
{
    return g_hInstance;
}

CIEBrowserEngine* CIEBrowserEngine::getInstance(HWND hParentWnd, HINSTANCE hInstance)
{
    if (!g_hInstance)
    {
        g_hInstance = new CIEBrowserEngine(hParentWnd, hInstance);
    }

    return g_hInstance;
}

//////////////////////////////////////////////////////////////////////////

CIEBrowserEngine::CIEBrowserEngine(HWND hParentWnd, HINSTANCE hInstance) :
        m_parentHWND(NULL),
        m_hparentInst(NULL),
        m_bLoadingComplete(FALSE),
        m_hNavigated(NULL),
        m_dwNavigationTimeout(0)
{
    m_parentHWND = hParentWnd;    
    m_hparentInst = hInstance;
    m_tabID = 0; //tabID;

    GetWindowRect(hParentWnd, &m_rcViewSize);

    m_tcNavigatedURL[0] = 0;

    CreateEngine();
}

CIEBrowserEngine::~CIEBrowserEngine()
{
    //  Destroy the Browser Object
    DestroyWindow(m_hwndTabHTML);
    m_hwndTabHTML = NULL;

    //  Destroy the Browser Object's parent if it exists
    if (g_hwndTabHTMLContainer)
    {
        DestroyWindow(g_hwndTabHTMLContainer);
        g_hwndTabHTMLContainer = NULL;
    }
}

HRESULT CIEBrowserEngine::RegisterWindowClass(HINSTANCE hInstance, WNDPROC appWndProc) 
{
    WNDCLASS    wc = { 0 };
    HRESULT     hrResult = 0;

    if (!GetClassInfo(hInstance, HTML_CONTAINER_NAME, &wc))
    {
        wc.style            = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc      = appWndProc;
        wc.hInstance        = hInstance;
        wc.hIcon			= NULL;
        wc.lpszClassName    = HTML_CONTAINER_NAME;
        wc.hCursor          = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground    = (HBRUSH)(COLOR_WINDOW + 1);

        hrResult = (RegisterClass(&wc) ? S_OK : E_FAIL);
    }
    else
        hrResult = S_OK;

    return hrResult;
}

LRESULT CIEBrowserEngine::CreateEngine()
{
    //  Create an HTML container to listen for Engine Events if one does not 
    //  already exist
    if (g_hwndTabHTMLContainer == NULL)
    {
        //register the main window
        if (FAILED(RegisterWindowClass(m_hparentInst, &CIEBrowserEngine::WndProc)))
            return S_FALSE;

        //create the main window
        g_hwndTabHTMLContainer = CreateWindowEx( 0, HTML_CONTAINER_NAME, NULL, 
            WS_POPUP | WS_VISIBLE, 
            CW_USEDEFAULT, CW_USEDEFAULT, 
            CW_USEDEFAULT, CW_USEDEFAULT, 
            m_parentHWND, NULL, m_hparentInst, 0);

        if(!g_hwndTabHTMLContainer)
            return S_FALSE;


        if (FAILED(rho::browser::InitHTMLControl(m_hparentInst)))
            return S_FALSE;
    }

    m_hwndTabHTML = CreateWindow(WC_HTML, NULL, 
        WS_POPUP | WS_VISIBLE | HS_NOSELECTION, 
        m_rcViewSize.left, m_rcViewSize.top, 
        (m_rcViewSize.right-m_rcViewSize.left), 
        (m_rcViewSize.bottom-m_rcViewSize.top), 
        g_hwndTabHTMLContainer, NULL, m_hparentInst, NULL);    

    if (!m_hwndTabHTML)
        return S_FALSE;

    CloseHandle (CreateThread(NULL, 0, 
        &CIEBrowserEngine::RegisterWndProcThread, (LPVOID)this, 0, NULL));

    return S_OK;
}

BOOL CIEBrowserEngine::Navigate(LPCTSTR tcURL, int iTabID)
{
    //setting the navigation timeout to default value
    setNavigationTimeout(45000);
    //  On Windows Mobile devices it has been observed that attempting to 
    //  navigate to a Javascript function before the page is fully loaded can 
    //  crash PocketBrowser (specifically when using Reload).  This condition
    //  prevents that behaviour.
    
    if (!m_bLoadingComplete && (wcsnicmp(tcURL, L"JavaScript:", wcslen(L"JavaScript:")) == 0))
    {
        LOG(TRACE) + "Failed to Navigate, Navigation in Progress\n";
        return S_FALSE;
    }

    LRESULT retVal = S_FALSE;

    if (wcsicmp(tcURL, L"history:back") == 0)
    {
    }
    else
    {
        //  Engine component does not accept Navigate(page.html), it needs
        //  the absolute URL of the page, add that here (if the user puts a .\ before)
        TCHAR tcDereferencedURL[MAX_URL];
        memset(tcDereferencedURL, 0, MAX_URL * sizeof(TCHAR));
        if (rho::browser::IsRelativeURL(tcURL))
        {
            if (!rho::browser::DereferenceURL(tcURL, tcDereferencedURL, m_tcNavigatedURL))
                return S_FALSE;
        }
        else
            wcscpy(tcDereferencedURL, tcURL);

        //  Test to see if the navigation URL starts with a '\', if it does
        //  then prepend 'file://'
        if (tcDereferencedURL[0] == L'\\')
        {
            if (wcslen(tcDereferencedURL) <= (MAX_URL - wcslen(L"file://")))
            {
                TCHAR tcNewURL[MAX_URL + 1];
                wsprintf(tcNewURL, L"file://%s", tcDereferencedURL);
                retVal = SendMessage(m_hwndTabHTML, DTM_NAVIGATE, 0, (LPARAM) (LPCTSTR)tcNewURL);
            }
        }
        else if (wcslen(tcDereferencedURL) > wcslen(L"www") && wcsnicmp(tcURL, L"www", 3) == 0)
        {
            if (wcslen(tcDereferencedURL) <= (MAX_URL - wcslen(L"http://")))
            {
                TCHAR tcNewURL[MAX_URL + 1];
                wsprintf(tcNewURL, L"http://%s", tcDereferencedURL);
                retVal = SendMessage(m_hwndTabHTML, DTM_NAVIGATE, 0, (LPARAM) (LPCTSTR)tcNewURL);
            }
        }
        else
            retVal = SendMessage(m_hwndTabHTML, DTM_NAVIGATE, 0, (LPARAM) (LPCTSTR)tcDereferencedURL);
    }

    return retVal;
}

BOOL CIEBrowserEngine::ResizeOnTab(int iInstID,RECT rcNewSize)
{    
    m_rcViewSize = rcNewSize;

    if(!m_bLoadingComplete)
        return TRUE;

    if (MoveWindow(m_hwndTabHTML,
                   m_rcViewSize.left, 
                   m_rcViewSize.top, 
                  (m_rcViewSize.right-m_rcViewSize.left), 
                  (m_rcViewSize.bottom-m_rcViewSize.top), 
                   FALSE))
    {
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}

BOOL CIEBrowserEngine::BackOnTab(int iInstID,int iPagesBack /*= 1*/)
{
    return Navigate(L"history:back", 0);
}

BOOL CIEBrowserEngine::ForwardOnTab(int iInstID)
{
    return TRUE;
}

BOOL CIEBrowserEngine::ReloadOnTab(bool bFromCache, UINT iTab)
{
    return TRUE; 
}

BOOL CIEBrowserEngine::StopOnTab(UINT iTab)
{
    return SendMessage(m_hwndTabHTML, DTM_STOP, 0, 0);
}

BOOL CIEBrowserEngine::ZoomPageOnTab(float fZoom, UINT iTab)
{
    return SendMessage(m_hwndTabHTML, DTM_ZOOMLEVEL, 0, fZoom);
}

BOOL CIEBrowserEngine::GetTitleOnTab(LPTSTR szURL, UINT iMaxLen, UINT iTab)
{
    return FALSE;
}

void CIEBrowserEngine::RunMessageLoop(CMainWindow& mainWnd)
{
	MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
		if (RHODESAPP().getExtManager().onWndMsg(msg) )
            continue;

		IDispatch* pDisp;
		SendMessage(m_hwndTabHTML, DTM_BROWSERDISPATCH, 0, (LPARAM) &pDisp); // New HTMLVIEW message
		if (pDisp != NULL) {
			//  If the Key is back we do not want to translate it causing the browser
			//  to navigate back.
			if ( ((msg.message != WM_KEYUP) && (msg.message != WM_KEYDOWN)) || (msg.wParam != VK_BACK) )
			{
				IOleInPlaceActiveObject* pInPlaceObject;
				pDisp->QueryInterface( IID_IOleInPlaceActiveObject, (void**)&pInPlaceObject );
				HRESULT handleKey = pInPlaceObject->TranslateAccelerator(&msg);	
			}
		}

        if (!mainWnd.TranslateAccelerator(&msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

		if(msg.message == WM_PAINT)
			RHODESAPP().getExtManager().onHTMLWndMsg(msg);	
    }
}

void CIEBrowserEngine::executeJavascript(const wchar_t* szJSFunction, int index)
{
    //  Test to see if the passed function starts with "JavaScript:" and 
    //  if it does not then prepend it.
    if (_memicmp(szJSFunction, L"JavaScript:", 22))
    {
        //  Function does not start with JavaScript:
        TCHAR* tcURI = new TCHAR[MAX_URL];
        wsprintf(tcURI, L"JavaScript:%s", szJSFunction);
        LRESULT retVal;
        retVal = Navigate(tcURI, 0);
        delete[] tcURI;
    }
    else
    {
        Navigate(szJSFunction, 0);
    }
}

void CIEBrowserEngine::OnDocumentComplete(LPCTSTR url)
{
    if(!m_bLoadingComplete && wcscmp(url,_T("about:blank")) != 0)
    {
        m_bLoadingComplete = true;

        ResizeOnTab(0, m_rcViewSize);
    }
}

void CIEBrowserEngine::setNavigationTimeout(unsigned int dwMilliseconds)
{
    m_dwNavigationTimeout = dwMilliseconds;
}

HWND CIEBrowserEngine::GetHTMLWND(int /*iTabID*/)
{
    return m_hwndTabHTML;
}

LRESULT CIEBrowserEngine::OnWebKitMessages(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
    bHandled = TRUE;

    switch (uMsg) 
    {
    case PB_ONMETA:
        {
            EngineMETATag* metaTag2 = (EngineMETATag*)lParam;

            RHODESAPP().getExtManager().onSetPropertiesData( (LPCWSTR)wParam, (LPCWSTR)lParam );
            
            if (metaTag2 && wcscmp(metaTag2->tcHTTPEquiv, L"initialiseRhoElementsExtension") != 0)
            {
                free(metaTag2->tcHTTPEquiv);
                free(metaTag2->tcContents);
                delete metaTag2;
            }
        }
        break;
    case PB_ONTOPMOSTWINDOW:
        LOG(INFO) + "START PB_ONTOPMOSTWINDOW";
        LRESULT rtRes = rho_wm_appmanager_ProcessOnTopMostWnd(wParam, lParam);
        LOG(INFO) + "END PB_ONTOPMOSTWINDOW";
        return rtRes;
        break;
    }

    return 0;
}

//////////////////////////////////////////////////////////////////////////
// invoke methods

void CIEBrowserEngine::InvokeEngineEventMetaTag(LPTSTR tcHttpEquiv, LPTSTR tcContent)
{
    EngineMETATag metaTag;
    LRESULT lRet;

    EngineMETATag *pMeta = new EngineMETATag;
	pMeta->tcHTTPEquiv = tcHttpEquiv;
	pMeta->tcContents = tcContent;

    if(pMeta->tcHTTPEquiv && *pMeta->tcHTTPEquiv){
        metaTag.tcHTTPEquiv = new WCHAR[wcslen(pMeta->tcHTTPEquiv)+1];
        wcscpy(metaTag.tcHTTPEquiv,pMeta->tcHTTPEquiv);
    }

    if(pMeta->tcContents && *pMeta->tcContents){
        metaTag.tcContents = new WCHAR[wcslen(pMeta->tcContents)+1];
        wcscpy(metaTag.tcContents,pMeta->tcContents);
    }

    EngineMETATag* metaTag2 = new EngineMETATag;
    metaTag2->tcHTTPEquiv = _tcsdup(metaTag.tcHTTPEquiv);
    metaTag2->tcContents = _tcsdup(metaTag.tcContents);
    lRet = PostMessage(rho_wmimpl_get_mainwnd(),PB_ONMETA,(WPARAM)m_tabID, (LPARAM)metaTag2);

    delete []metaTag.tcHTTPEquiv;
    delete []metaTag.tcContents;

    //delete [] pMeta->tcHTTPEquiv;
    //delete [] pMeta->tcContents;
    //delete pMeta;
}

void CIEBrowserEngine::InvokeEngineEventLoad(LPTSTR tcURL, EngineEventID eeEventID)
{
	//  Engine component has indicated a load event, this should be 
	//  one of BeforeNavigate, NavigateComplete or DocumentComplete.
	wcscpy(m_tcNavigatedURL, tcURL);

	switch (eeEventID)
	{
		case EEID_BEFORENAVIGATE:
			m_bLoadingComplete = FALSE;
			SetEvent(m_hNavigated);
			CloseHandle(m_hNavigated);
			m_hNavigated = NULL;

			//  Do not start the Navigation Timeout Timer if the 
			//  navigation request is a script call.
			if((!_memicmp(tcURL, L"javascript:", 11 * sizeof(TCHAR)))
				|| (!_memicmp(tcURL, L"jscript:", 8 * sizeof(TCHAR)))
				|| (!_memicmp(tcURL, L"vbscript:", 9 * sizeof(TCHAR)))
				|| (!_memicmp(tcURL, L"res://\\Windows\\shdoclc.dll/navcancl.htm", 35 * sizeof(TCHAR))))
			{
					break;
			}

			//  Test if the user has attempted to navigate back in the history
			if (wcsicmp(tcURL, L"history:back") == 0)
			{
			}
			
			CloseHandle (CreateThread(NULL, 0, 
									&CIEBrowserEngine::NavigationTimeoutThread, 
									(LPVOID)this, 0, NULL));

            PostMessage(m_parentHWND, WM_BROWSER_ONBEFORENAVIGATE, (WPARAM)m_tabID, (LPARAM)_tcsdup(tcURL));
			break;
		case EEID_DOCUMENTCOMPLETE:
			m_bLoadingComplete = TRUE;
            PostMessage(m_parentHWND, WM_BROWSER_ONDOCUMENTCOMPLETE, m_tabID, (LPARAM)_tcsdup(tcURL));
			break;
		case EEID_NAVIGATECOMPLETE:
			SetEvent(m_hNavigated);
			CloseHandle(m_hNavigated);
			m_hNavigated = NULL;
            SendMessage(m_parentHWND, WM_BROWSER_ONNAVIGATECOMPLETE, (WPARAM)m_tabID, (LPARAM)tcURL);			
			break;
	}
}

void CIEBrowserEngine::InvokeEngineEventTitleChange(LPTSTR tcTitle)
{
	////  Notify the core the page title has changed, if not specified between 
	////  <TITLE> </TITLE> tags this should be set to the page URL.
    TCHAR tcURL[MAX_URL];
    memset(tcURL, 0, sizeof(MAX_URL) * sizeof(TCHAR));
    wcsncpy(tcURL, tcTitle, MAX_URL);

    SendMessage(m_parentHWND, WM_BROWSER_ONTITLECHANGE, (WPARAM)m_tabID, (LPARAM)tcURL);
}

//////////////////////////////////////////////////////////////////////////
// system handlers

/**
* \author	Darryn Campbell (DCC, JRQ768)
* \date		October 2009
*/
LRESULT CIEBrowserEngine::WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	LRESULT lResult = S_OK;

    switch (uMsg) 
	{
		case WM_NOTIFY:
		{
			//  Received a message from the Pocket Internet Explorer component
			//  to indicate something has happened (BeforeNavigate / DocumentComplete etc).
			//  The lParam contains an NM_HTMLVIEWW containing information about
			//  the event, parse it.
			NM_HTMLVIEWW * pnmHTML;
			LPNMHDR pnmh;
			pnmHTML = (NM_HTMLVIEWW *) lParam;
			pnmh    = (LPNMHDR) &(pnmHTML->hdr);
			
			//  The message has originated from one of the tabs we created, determine
			//  the tab ID from the hwnd
			//IETab* tab = GetSpecificTab(hwndParentOfOriginator);
            CIEBrowserEngine* mobileTab = CIEBrowserEngine::getInstance();

			//  Invoke the appropriate tab with the event.  The data will vary
			//  depending on which event has been received
			TCHAR tcTarget[MAX_URL + 1];
			memset (tcTarget, 0, sizeof(TCHAR) * MAX_URL + 1);
			switch (pnmh->code)
			{
			case NM_PIE_TITLE:
				//  The Page Title has been received from the Page, convert 
				//  the content to a wide string
				if (pnmHTML->szTarget)
					mbstowcs(tcTarget, (LPSTR)pnmHTML->szTarget, MAX_URL);
				if (tcTarget)
					mobileTab->InvokeEngineEventTitleChange(tcTarget);
				break;
			case NM_PIE_META:

				//  A Meta Tag has been received from the Page, convert the content
				//  and data to wide strings.
				if (pnmHTML->szTarget)
					mbstowcs(tcTarget, (LPSTR)pnmHTML->szTarget, MAX_URL);
				TCHAR tcData[MAX_URL+1];
				memset(tcData, 0, sizeof(TCHAR) * MAX_URL + 1);
				if (pnmHTML->szData)
					mbstowcs(tcData, (LPSTR)pnmHTML->szData, MAX_URL);
				//  If there is both an HTTP Equiv and some Content to the Meta
				//  tag then invoke it
				if (tcTarget && tcData)
					mobileTab->InvokeEngineEventMetaTag(tcTarget, tcData);			
				break;
			case NM_PIE_BEFORENAVIGATE:
				if (pnmHTML->szTarget)
					mbstowcs(tcTarget, (LPSTR)pnmHTML->szTarget, MAX_URL);

				// GD - stop navigation if target starts with file:// and ends with '\'.
				// This is the generated target when using <a href=""> from a local page.
				// If we don't stop it then the File Explorer is launched.
				if (wcslen (tcTarget) >= 8)
					if (!wcsnicmp (tcTarget, L"file://", 7))
						if (tcTarget [wcslen (tcTarget) - 1] == '\\')
						{
							LOG(TRACE) + "Navigation to file folder aborted\n";
							return S_FALSE;
						}
				if (tcTarget)
					mobileTab->InvokeEngineEventLoad(tcTarget, EEID_BEFORENAVIGATE);
				break;
			case NM_PIE_DOCUMENTCOMPLETE:
				if (pnmHTML->szTarget)
					mbstowcs(tcTarget, (LPSTR)pnmHTML->szTarget, MAX_URL);
				//  If the network is available but the server being reached
				//  is inaccessible the browser component appears to immediately
				//  give a document complete with the current page URL (not the
				//  page being navigated to) which is hiding the hourglass, 
				//  stop this behaviour.
				if (tcTarget && !wcsicmp(tcTarget, mobileTab->m_tcNavigatedURL))
					mobileTab->InvokeEngineEventLoad(tcTarget, EEID_DOCUMENTCOMPLETE);
				break;
			case NM_PIE_NAVIGATECOMPLETE:
				if (pnmHTML->szTarget)
					mbstowcs(tcTarget, (LPSTR)pnmHTML->szTarget, MAX_URL);
				if (tcTarget)
					mobileTab->InvokeEngineEventLoad(tcTarget, EEID_NAVIGATECOMPLETE);
				if(mobileTab->m_hNavigated!=NULL)
					SetEvent(mobileTab->m_hNavigated);

				break;
			case NM_PIE_KEYSTATE:
			case NM_PIE_ALPHAKEYSTATE:
				//  Not Used
				break;

			}
		}	

		lResult = DefWindowProc(hwnd, uMsg, wParam, lParam);
	}

	return lResult;
}

DWORD WINAPI CIEBrowserEngine::NavigationTimeoutThread( LPVOID lpParameter )
{
    CIEBrowserEngine* pIEEng = reinterpret_cast<CIEBrowserEngine*>(lpParameter);
    DWORD dwWaitResult;
    if (pIEEng->m_dwNavigationTimeout != 0)
    {
        LOG(TRACE) + "Mobile NavThread Started\n";

	    if(pIEEng->m_hNavigated==NULL)
		    pIEEng->m_hNavigated = CreateEvent(NULL, TRUE, FALSE, L"PB_IEENGINE_NAVIGATION_IN_PROGRESS");
		dwWaitResult = WaitForSingleObject(pIEEng->m_hNavigated, pIEEng->m_dwNavigationTimeout);

		switch (dwWaitResult) 
		{
			// Event object was signaled
			case WAIT_OBJECT_0: 
				//
				// TODO: Read from the shared buffer
				//
				LOG(INFO) + "NavigationTimeoutThread:Event object was signaled\n";
				break; 
			case WAIT_TIMEOUT: 
				//
				// TODO: Read from the shared buffer
				//
				LOG(INFO) + "NavigationTimeoutThread:timeout\n";
				
					
				pIEEng->StopOnTab(0);
				SendMessage(pIEEng->m_parentHWND, WM_BROWSER_ONNAVIGATIONTIMEOUT, 
					(WPARAM)pIEEng->m_tabID, (LPARAM)pIEEng->m_tcNavigatedURL);

				break; 

			// An error occurred
			default: 
				LOG(INFO) + "Wait error  GetLastError()=\n"+ GetLastError();
				return 0; 
		}



		CloseHandle(pIEEng->m_hNavigated);
		pIEEng->m_hNavigated = NULL;

	    LOG(TRACE) + "NavThread Ended\n";
}

BOOL CIEBrowserEngine::ZoomTextOnTab(int nZoom, UINT iTab)
{
	BOOL bRetVal = PostMessage(m_hwndTabHTML, DTM_ZOOMLEVEL, 0, 
								(LPARAM)(DWORD) nZoom);

	if (bRetVal)
	{
		//m_dwCurrentTextZoomLevel = dwZoomLevel;
		return S_OK;
	}
	else
		return S_FALSE;
}

#define  PB_ENGINE_IE_MOBILE

DWORD WINAPI CIEBrowserEngine::RegisterWndProcThread(LPVOID lpParameter)
{
    //  We are passed a pointer to the engine we are interested in.
    CIEBrowserEngine* pEngine = (CIEBrowserEngine*)lpParameter;

    //  The window tree appears as follows on CE:
    //  +--m_htmlHWND
    //     |
    //     +--child of m_htmlHWND
    //        |
    //        +--child which receives Windows Messages

    //  The window tree appears as follows on WM:
    //  +--m_htmlHWND
    //     |
    //     +--child of m_htmlHWND which receives Windows Messages

    //  Obtain the created HTML HWND
    HWND hwndHTML = NULL;
    HWND hwndHTMLMessageWindow = NULL;

    while (hwndHTMLMessageWindow == NULL)
    {
        hwndHTML = pEngine->GetHTMLWND(0);

        if (hwndHTML != NULL)
        {
            hwndHTMLMessageWindow = GetWindow(hwndHTML, GW_CHILD);
        }

        //  Failed to find the desired child window, take a short nap before 
        //  trying again as the window is still creating.
        Sleep(100);
    }

    PostMessage(rho_wmimpl_get_mainwnd(), PB_ONTOPMOSTWINDOW,(LPARAM)0, (WPARAM)hwndHTMLMessageWindow);

    return 0;
}
