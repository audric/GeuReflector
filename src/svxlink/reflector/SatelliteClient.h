#ifndef SATELLITE_CLIENT_INCLUDED
#define SATELLITE_CLIENT_INCLUDED

#include <string>
#include <vector>
#include <sigc++/sigc++.h>
#include <AsyncConfig.h>
#include <AsyncTcpPrioClient.h>
#include <AsyncFramedTcpConnection.h>
#include <AsyncTimer.h>

#include "TgFilter.h"
#include "ReflectorMsg.h"

class Reflector;

/**
@brief  Satellite-side outbound connection to a parent reflector

When SATELLITE_OF is configured, the reflector runs in satellite mode:
it connects to a parent reflector and relays all local client events.
Events received from the parent are broadcast to local clients.
No prefix logic, no trunk mesh participation.

An optional SATELLITE_FILTER scopes which TGs this satellite cares about,
in both directions (TgFilter syntax: exact, prefix "24*", range
"2427-2438", comma-separated):
 - outbound: local events for non-matching TGs are not sent to the parent;
 - inbound:  the filter is sent to the parent via MsgPeerFilter (type 122)
   after authentication; the parent skips forwarding non-matching TGs.
*/
class SatelliteClient : public sigc::trackable
{
  public:
    SatelliteClient(Reflector* reflector, Async::Config& cfg);
    ~SatelliteClient(void);

    bool initialize(void);

    // Called by Reflector when a local client starts/stops talking
    void onLocalTalkerStart(uint32_t tg, const std::string& callsign);
    void onLocalTalkerStop(uint32_t tg);
    void onLocalAudio(uint32_t tg, const std::vector<uint8_t>& audio);
    void onLocalFlush(uint32_t tg);

    // Send our local roster up to the parent. Caller has already filtered
    // to "this satellite's local clients" (sat_id empty on the wire).
    void sendNodeList(
        const std::vector<MsgPeerNodeList::NodeEntry>& nodes);

    // Read-only access to the parent's combined-view roster, surfaced
    // by Reflector::statusJson under /status.parent.nodes.
    const std::vector<MsgPeerNodeList::NodeEntry>& parentNodes(void) const
    {
      return m_parent_nodes;
    }
    const std::string& parentId(void) const { return m_parent_id; }

  private:
    static const unsigned HEARTBEAT_TX_CNT_RESET = 10;
    static const unsigned HEARTBEAT_RX_CNT_RESET = 15;

    using FramedTcpClient =
        Async::TcpPrioClient<Async::FramedTcpConnection>;

    Reflector*      m_reflector;
    Async::Config&  m_cfg;
    std::string     m_parent_host;
    uint16_t        m_parent_port;
    std::string     m_secret;
    std::string     m_satellite_id;
    uint32_t        m_priority;
    bool            m_hello_received;
    FramedTcpClient m_con;
    Async::Timer    m_heartbeat_timer;
    unsigned        m_hb_tx_cnt;
    unsigned        m_hb_rx_cnt;
    TgFilter        m_filter;       // optional TG filter (SATELLITE_FILTER)
    std::string     m_filter_str;   // raw config string for MsgPeerFilter
    // Parent's id from MsgPeerHello (used as the peer_id key for Redis
    // tombstone bookkeeping when storing the parent-supplied roster).
    std::string     m_parent_id;
    // Parent's combined roster (parent-local + every sibling satellite,
    // minus our own contribution). Surfaced via /status.parent.nodes.
    std::vector<MsgPeerNodeList::NodeEntry> m_parent_nodes;

    void onConnected(void);
    void onDisconnected(Async::TcpConnection* con,
                        Async::TcpConnection::DisconnectReason reason);
    void onFrameReceived(Async::FramedTcpConnection* con,
                         std::vector<uint8_t>& data);
    void handleMsgPeerHello(std::istream& is);
    void handleMsgPeerTalkerStart(std::istream& is);
    void handleMsgPeerTalkerStop(std::istream& is);
    void handleMsgPeerAudio(std::istream& is);
    void handleMsgPeerFlush(std::istream& is);
    void handleMsgPeerHeartbeat(void);
    void handleMsgPeerNodeList(std::istream& is);
    void sendMsg(const ReflectorMsg& msg);
    void sendFilter(void);
    void heartbeatTick(Async::Timer* t);
};

#endif /* SATELLITE_CLIENT_INCLUDED */
