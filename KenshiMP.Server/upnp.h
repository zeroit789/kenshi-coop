// ES: upnp.h - UPnPMapper: abre automáticamente el puerto del servidor en el router usando la
//     API COM de Windows (IUPnPNAT / IStaticPortMappingCollection), sin dependencias externas.
//     GameServer::Start lo usa antes de escuchar; si falla, cae a una regla de firewall con netsh.
// EN: upnp.h - UPnPMapper: automatically opens the server port on the router through the
//     Windows COM API (IUPnPNAT / IStaticPortMappingCollection), with no external dependencies.
//     GameServer::Start uses it before listening; on failure it falls back to a netsh firewall rule.
#pragma once
#include <cstdint>
#include <string>

namespace kmp {

// ES: Mapeo automático de puertos por UPnP con la API COM integrada en Windows.
// EN: UPnP auto port mapping using Windows built-in COM API.
// ES: Sin dependencias externas: usa IUPnPNAT / IStaticPortMappingCollection.
// EN: No external dependencies — uses IUPnPNAT / IStaticPortMappingCollection.

class UPnPMapper {
public:
    // ES: Intenta mapear puertoExterno -> IPlocal:puertoInterno por UPnP.
    //     protocol: "UDP" o "TCP". Devuelve true si lo consigue (y guarda el mapeo para quitarlo luego).
    // EN: Attempt to map externalPort -> localIP:internalPort via UPnP.
    // protocol: "UDP" or "TCP"
    // Returns true on success.
    bool AddMapping(uint16_t externalPort, uint16_t internalPort,
                    const std::string& protocol, const std::string& description);

    // ES: Quita un mapeo añadido antes (no hace nada si no había mapeo).
    // EN: Remove a previously added mapping.
    bool RemoveMapping(uint16_t externalPort, const std::string& protocol);

    // ES: IP externa (WAN) leída del router a través de nuestro mapeo; "" si no está disponible.
    // EN: Get the external (WAN) IP address from the router, if available.
    std::string GetExternalIP();

    // ES: IP local de la LAN usando un socket UDP de prueba (no envía tráfico).
    // EN: Get local LAN IP by creating a dummy UDP socket.
    static std::string GetLocalIP();

    // ES: true si hay un mapeo activo creado por esta instancia.
    // EN: true if there is an active mapping created by this instance.
    bool IsMapped() const { return m_mapped; }

private:
    // ES: Estado del mapeo actual (puerto y protocolo) para poder leerlo o quitarlo después.
    // EN: Current mapping state (port and protocol) so it can be read or removed later.
    bool m_mapped = false;
    uint16_t m_mappedPort = 0;
    std::string m_mappedProtocol;
};

} // namespace kmp
