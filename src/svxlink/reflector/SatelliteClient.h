#ifndef SATELLITE_CLIENT_INCLUDED
#define SATELLITE_CLIENT_INCLUDED

#include <string>
#include <vector>
#include <sigc++/sigc++.h>
#include <AsyncConfig.h>
#include <AsyncTcpPrioClient.h>
#include <AsyncFramedTcpConnection.h>
#include <AsyncTimer.h>

class Reflector;
class ReflectorMsg;

/**
@brief  Satellite-side outbound connection to a parent reflector

When SATELLITE_OF is configured, the reflector runs in satellite mode:
it connects to a parent reflector and relays all local client events.
Events received from the parent are broadcast to local clients.
No prefix logic, no trunk mesh participation.
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

    void onConnected(void);
    void onDisconnected(Async::TcpConnection* con,
                        Async::TcpConnection::DisconnectReason reason);
    void onFrameReceived(Async::FramedTcpConnection* con,
                         std::vector<uint8_t>& data);
    void handleMsgTrunkHello(std::istream& is);
    void handleMsgTrunkTalkerStart(std::istream& is);
    void handleMsgTrunkTalkerStop(std::istream& is);
    void handleMsgTrunkAudio(std::istream& is);
    void handleMsgTrunkFlush(std::istream& is);
    void handleMsgTrunkHeartbeat(void);
    void sendMsg(const ReflectorMsg& msg);
    void heartbeatTick(Async::Timer* t);
};

#endif /* SATELLITE_CLIENT_INCLUDED */
