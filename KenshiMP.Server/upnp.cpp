// ES: upnp.cpp - Implementación de UPnPMapper con COM (natupnp.h) y WinSock para la IP local.
// EN: upnp.cpp - UPnPMapper implementation using COM (natupnp.h) and WinSock for the local IP.
#include "upnp.h"
#include <spdlog/spdlog.h>

#include <Windows.h>
#include <natupnp.h>
#include <comdef.h>
#include <WinSock2.h>
#include <WS2tcpip.h>

// ES: Enlaza las bibliotecas COM necesarias (CoCreateInstance, BSTR).
// EN: Links the required COM libraries (CoCreateInstance, BSTR).
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace kmp {

// ES: Averigua la IP local de la interfaz de salida. Si algo falla devuelve "127.0.0.1".
// EN: Finds the local IP of the outgoing interface. Returns "127.0.0.1" if anything fails.
std::string UPnPMapper::GetLocalIP() {
    // ES: Crea un socket UDP y lo "conecta" a una IP pública (8.8.8.8) para que el sistema elija la
    //     ruta local; getsockname da entonces nuestra IP. No se envía tráfico real.
    // EN: Create a UDP socket and "connect" to a public IP to determine local route.
    // No actual traffic is sent.
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return "127.0.0.1";

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return "127.0.0.1";
    }

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(80);
    inet_pton(AF_INET, "8.8.8.8", &target.sin_addr);

    if (connect(sock, (sockaddr*)&target, sizeof(target)) != 0) {
        closesocket(sock);
        WSACleanup();
        return "127.0.0.1";
    }

    sockaddr_in local{};
    int localLen = sizeof(local);
    if (getsockname(sock, (sockaddr*)&local, &localLen) != 0) {
        closesocket(sock);
        WSACleanup();
        return "127.0.0.1";
    }

    char ipStr[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &local.sin_addr, ipStr, sizeof(ipStr));

    closesocket(sock);
    // ES: No llamar a WSACleanup aquí: el servidor necesita WinSock vivo para ENet.
    // EN: Don't WSACleanup here — server needs WinSock alive for ENet
    return ipStr;
}

// ES: Crea el objeto UPnPNAT, obtiene la colección de mapeos (con reintentos porque el
//     descubrimiento UPnP es lento), añade el mapeo hacia nuestra IP local y registra la IP externa.
//     Puede bloquear unos segundos (Sleep entre reintentos). Devuelve true si quedó mapeado.
// EN: Creates the UPnPNAT object, gets the mapping collection (retrying because UPnP discovery
//     is slow), adds the mapping to our local IP and logs the external IP.
//     May block a few seconds (Sleep between retries). Returns true if mapped.
bool UPnPMapper::AddMapping(uint16_t externalPort, uint16_t internalPort,
                            const std::string& protocol, const std::string& description) {
    spdlog::info("UPnP: Attempting to map port {} ({})...", externalPort, protocol);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool needUninit = SUCCEEDED(hr);
    // ES: S_FALSE significa que COM ya estaba inicializado en este hilo, lo cual es correcto.
    // EN: S_FALSE means already initialized, which is fine

    IUPnPNAT* nat = nullptr;
    hr = CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL,
                          __uuidof(IUPnPNAT), (void**)&nat);
    if (FAILED(hr) || !nat) {
        spdlog::warn("UPnP: Failed to create UPnPNAT instance (hr=0x{:08X})", (unsigned)hr);
        spdlog::warn("UPnP: Router may not support UPnP. Players will need to forward port {} manually.", externalPort);
        if (needUninit) CoUninitialize();
        return false;
    }

    IStaticPortMappingCollection* mappings = nullptr;

    // ES: El descubrimiento UPnP puede ser lento: hasta 3 intentos con 1 s de espera.
    // EN: The UPnP discovery can be slow — retry a few times
    for (int attempt = 0; attempt < 3; attempt++) {
        hr = nat->get_StaticPortMappingCollection(&mappings);
        if (SUCCEEDED(hr) && mappings) break;
        spdlog::debug("UPnP: Discovery attempt {} failed, retrying...", attempt + 1);
        Sleep(1000);
    }

    if (FAILED(hr) || !mappings) {
        spdlog::warn("UPnP: Could not get port mapping collection (hr=0x{:08X})", (unsigned)hr);
        spdlog::warn("UPnP: UPnP may be disabled on router. Forward port {} manually.", externalPort);
        nat->Release();
        if (needUninit) CoUninitialize();
        return false;
    }

    // ES: IP local a la que apuntará el mapeo.
    // EN: Get local IP
    std::string localIP = GetLocalIP();
    spdlog::info("UPnP: Local IP is {}", localIP);

    // ES: Convierte las cadenas a BSTR (formato de texto de COM).
    // EN: Convert strings to BSTR
    std::wstring wProtocol(protocol.begin(), protocol.end());
    std::wstring wLocalIP(localIP.begin(), localIP.end());
    std::wstring wDescription(description.begin(), description.end());

    BSTR bstrProtocol = SysAllocString(wProtocol.c_str());
    BSTR bstrLocalIP = SysAllocString(wLocalIP.c_str());
    BSTR bstrDescription = SysAllocString(wDescription.c_str());

    // ES: Añade el mapeo (habilitado) y libera los BSTR.
    // EN: Adds the (enabled) mapping and frees the BSTRs.
    IStaticPortMapping* mapping = nullptr;
    hr = mappings->Add(externalPort, bstrProtocol, internalPort,
                       bstrLocalIP, VARIANT_TRUE, bstrDescription, &mapping);

    SysFreeString(bstrProtocol);
    SysFreeString(bstrLocalIP);
    SysFreeString(bstrDescription);

    if (SUCCEEDED(hr) && mapping) {
        // ES: Intenta leer la IP externa desde el mapeo recién creado (solo para el log).
        // EN: Try to get external IP from the mapping
        BSTR bstrExternalIP = nullptr;
        if (SUCCEEDED(mapping->get_ExternalIPAddress(&bstrExternalIP)) && bstrExternalIP) {
            int len = WideCharToMultiByte(CP_UTF8, 0, bstrExternalIP, -1, nullptr, 0, nullptr, nullptr);
            std::string extIP(len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, bstrExternalIP, -1, extIP.data(), len, nullptr, nullptr);
            spdlog::info("UPnP: External IP is {}", extIP);
            SysFreeString(bstrExternalIP);
        }
        mapping->Release();

        m_mapped = true;
        m_mappedPort = externalPort;
        m_mappedProtocol = protocol;

        spdlog::info("UPnP: Successfully mapped port {} -> {}:{} ({})",
                     externalPort, localIP, internalPort, protocol);
    } else {
        spdlog::warn("UPnP: Failed to add port mapping (hr=0x{:08X})", (unsigned)hr);
        spdlog::warn("UPnP: Port {} may already be mapped or router denied the request.", externalPort);
    }

    mappings->Release();
    nat->Release();
    // ES: No llamar a CoUninitialize: COM queda vivo para llamadas posteriores (RemoveMapping, GetExternalIP).
    // EN: Don't CoUninitialize — keep COM alive for potential later calls

    return m_mapped;
}

// ES: Quita el mapeo del router (se llama desde GameServer::Stop). Si no había mapeo, devuelve true.
// EN: Removes the mapping from the router (called from GameServer::Stop). Returns true if there was none.
bool UPnPMapper::RemoveMapping(uint16_t externalPort, const std::string& protocol) {
    if (!m_mapped) return true; // Nothing to remove

    spdlog::info("UPnP: Removing port mapping {} ({})...", externalPort, protocol);

    IUPnPNAT* nat = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL,
                                  __uuidof(IUPnPNAT), (void**)&nat);
    if (FAILED(hr) || !nat) return false;

    IStaticPortMappingCollection* mappings = nullptr;
    hr = nat->get_StaticPortMappingCollection(&mappings);
    if (FAILED(hr) || !mappings) {
        nat->Release();
        return false;
    }

    std::wstring wProtocol(protocol.begin(), protocol.end());
    BSTR bstrProtocol = SysAllocString(wProtocol.c_str());

    hr = mappings->Remove(externalPort, bstrProtocol);

    SysFreeString(bstrProtocol);
    mappings->Release();
    nat->Release();

    if (SUCCEEDED(hr)) {
        spdlog::info("UPnP: Port mapping removed");
        m_mapped = false;
        return true;
    } else {
        spdlog::warn("UPnP: Failed to remove mapping (hr=0x{:08X})", (unsigned)hr);
        return false;
    }
}

// ES: Lee la IP externa desde nuestro propio mapeo (hace falta al menos uno). "" si no hay mapeo o falla.
//     GameServer la usa para anunciarse en el master server.
// EN: Reads the external IP from our own mapping (at least one is required). "" if none or on failure.
//     GameServer uses it to announce itself to the master server.
std::string UPnPMapper::GetExternalIP() {
    IUPnPNAT* nat = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL,
                                  __uuidof(IUPnPNAT), (void**)&nat);
    if (FAILED(hr) || !nat) return "";

    IStaticPortMappingCollection* mappings = nullptr;
    hr = nat->get_StaticPortMappingCollection(&mappings);
    if (FAILED(hr) || !mappings) {
        nat->Release();
        return "";
    }

    // ES: Hace falta al menos un mapeo para leer la IP externa: se intenta con el nuestro.
    // EN: We need at least one mapping to read the external IP from
    // Try to get our own mapping
    if (m_mapped) {
        std::wstring wProtocol(m_mappedProtocol.begin(), m_mappedProtocol.end());
        BSTR bstrProtocol = SysAllocString(wProtocol.c_str());

        IStaticPortMapping* mapping = nullptr;
        hr = mappings->get_Item(m_mappedPort, bstrProtocol, &mapping);
        SysFreeString(bstrProtocol);

        if (SUCCEEDED(hr) && mapping) {
            BSTR bstrIP = nullptr;
            if (SUCCEEDED(mapping->get_ExternalIPAddress(&bstrIP)) && bstrIP) {
                int len = WideCharToMultiByte(CP_UTF8, 0, bstrIP, -1, nullptr, 0, nullptr, nullptr);
                std::string ip(len - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, bstrIP, -1, ip.data(), len, nullptr, nullptr);
                SysFreeString(bstrIP);
                mapping->Release();
                mappings->Release();
                nat->Release();
                return ip;
            }
            mapping->Release();
        }
    }

    mappings->Release();
    nat->Release();
    return "";
}

} // namespace kmp
