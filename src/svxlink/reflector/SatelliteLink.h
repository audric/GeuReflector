#ifndef SATELLITE_LINK_INCLUDED
#define SATELLITE_LINK_INCLUDED

#include <set>
#include <string>
#include <vector>
#include <sigc++/sigc++.h>
#include <json/json.h>
#include <AsyncFramedTcpConnection.h>
#include <AsyncTimer.h>

class Reflector;
class ReflectorMsg;

/**
@brief  Handles one inbound satellite connection on the parent reflector

A SatelliteLink is created for each satellite that connects to the parent's
satellite port. It relays ALL TGs bidirectionally (no prefix filtering).
The parent always wins talker arbitration over satellites.
*/
class SatelliteLink : public sigc::trackable
{
  public:
    SatelliteLink(Reflector* reflector, Async::FramedTcpConnection* con,
                  const std::string& secret);
    ~SatelliteLink(void);

    bool isAuthenticated(void) const { return m_hello_received; }
    const std::string& satelliteId(void) const { return m_satellite_id; }
    Json::Value statusJson(void) const;

    /**
     * Emitted when the satellite link has failed (heartbeat timeout).
     * The Reflector must handle cleanup — the SatelliteLink stops its own
     * timer but does NOT call m_con->disconnect() itself.
     */
    sigc::signal<void, SatelliteLink*> linkFailed;

    /**
     * Emitted whenever the per-satellite status (id, authentication,
     * active TGs) changes. Reflector uses this to refresh the satellite's
     * Redis snapshot. Carries `this`; ignore if satelliteId() is empty
     * (hello not yet received).
     */
    sigc::signal<void, SatelliteLink*> statusChanged;

    // Events from local clients or trunk peers → send down to satellite
    void onParentTalkerStart(uint32_t tg, const std::string& callsign);
    void onParentTalkerStop(uint32_t tg);
    void onParentAudio(uint32_t tg, const std::vector<uint8_t>& audio);
    void onParentFlush(uint32_t tg);

  private:
    static const unsigned HEARTBEAT_TX_CNT_RESET = 10;
    static const unsigned HEARTBEAT_RX_CNT_RESET = 15;

    Reflector*                  m_reflector;
    Async::FramedTcpConnection* m_con;
    std::string                 m_secret;
    std::string                 m_satellite_id;
    bool                        m_hello_received;
    Async::Timer                m_heartbeat_timer;
    unsigned                    m_hb_tx_cnt;
    unsigned                    m_hb_rx_cnt;
    std::set<uint32_t>          m_sat_active_tgs;

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

#endif /* SATELLITE_LINK_INCLUDED */
