/**
@file    TwinLink.h
@brief   HA-pair link between two reflectors sharing a LOCAL_PREFIX
@author  GeuReflector
@date    2026-04-15

The twin link mirrors the full TGHandler state between two paired
reflectors (see docs/TWIN_PROTOCOL.md).  Unlike TrunkLink there is
no prefix filtering, no TG ownership arbitration, and no cluster-TG
logic — the pair shares everything.
*/

#ifndef TWINLINK_INCLUDED
#define TWINLINK_INCLUDED

#include <string>
#include <cstdint>
#include <vector>

#include <AsyncConfig.h>
#include <AsyncTcpPrioClient.h>
#include <AsyncFramedTcpConnection.h>
#include <AsyncTimer.h>

class Reflector;
class MsgTrunkHello;
class ReflectorMsg;

class TwinLink
{
  public:
    TwinLink(Reflector* reflector, Async::Config& cfg);
    ~TwinLink(void);

    bool initialize(void);

    // Inbound path: the twin server accepted a connection and the hello
    // HMAC verified — hand it off here.
    void acceptInboundConnection(Async::FramedTcpConnection* con,
                                 const MsgTrunkHello& hello);

    // Called by Reflector when local TGHandler state changes
    void onLocalTalkerUpdated(uint32_t tg, const std::string& callsign);
    void onLocalAudio(uint32_t tg, const std::vector<uint8_t>& audio);
    void onLocalFlush(uint32_t tg);

    // Called by Reflector when any TrunkLink receives an external
    // trunk-talker start/stop (so we can mirror it to our partner).
    void onExternalTrunkTalkerStart(uint32_t tg,
                                    const std::string& peer_id,
                                    const std::string& callsign);
    void onExternalTrunkTalkerStop(uint32_t tg,
                                   const std::string& peer_id);

    bool isActive(void) const;
    const std::string& partnerHost(void) const { return m_peer_host; }
    uint16_t partnerPort(void) const { return m_peer_port; }

  private:
    using FramedTcpClient =
        Async::TcpPrioClient<Async::FramedTcpConnection>;

    static const unsigned TWIN_HB_TX_THRESHOLD = 2;  // 2s TX idle => send HB
    static const unsigned TWIN_HB_RX_THRESHOLD = 5;  // 5s RX idle => dead

    Reflector*                    m_reflector;
    Async::Config&                m_cfg;
    std::string                   m_peer_host;
    uint16_t                      m_peer_port;
    std::string                   m_secret;
    std::string                   m_local_prefix;     // our LOCAL_PREFIX
    std::string                   m_peer_id_config;   // section name = "TWIN"
    std::string                   m_peer_id_received;
    uint32_t                      m_priority;
    uint32_t                      m_peer_priority;
    FramedTcpClient               m_con;              // outbound
    Async::FramedTcpConnection*   m_inbound_con;      // accepted inbound
    Async::Timer                  m_heartbeat_timer;  // 1000 ms tick
    bool                          m_ob_hello_received;
    unsigned                      m_ob_hb_tx_cnt;
    unsigned                      m_ob_hb_rx_cnt;
    bool                          m_ib_hello_received;
    unsigned                      m_ib_hb_tx_cnt;
    unsigned                      m_ib_hb_rx_cnt;

    // No copy
    TwinLink(const TwinLink&);
    TwinLink& operator=(const TwinLink&);

    void onConnected(void);
    void onDisconnected(Async::TcpConnection* con,
                        Async::TcpConnection::DisconnectReason reason);
    void onFrameReceived(Async::FramedTcpConnection* con,
                         std::vector<uint8_t>& data);
    void handleMsgTrunkHello(std::istream& is, bool is_inbound);
    void handleMsgTrunkTalkerStart(std::istream& is);
    void handleMsgTrunkTalkerStop(std::istream& is);
    void handleMsgTrunkAudio(std::istream& is);
    void handleMsgTrunkFlush(std::istream& is);
    void handleMsgTrunkHeartbeat(void);
    void handleMsgTwinExtTalkerStart(std::istream& is);
    void handleMsgTwinExtTalkerStop(std::istream& is);
    void onHeartbeatTick(Async::Timer*);

    bool sendMsg(const ReflectorMsg& msg);
    void sendMsgOnOutbound(const ReflectorMsg& msg);
    void sendMsgOnInbound(const ReflectorMsg& msg);
};

#endif /* TWINLINK_INCLUDED */
