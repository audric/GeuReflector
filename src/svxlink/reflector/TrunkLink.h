/**
@file    TrunkLink.h
@brief   Server-to-server trunk link between two SvxReflector instances
@date    2026-03-20

\verbatim
SvxReflector - An audio reflector for connecting SvxLink Servers
Copyright (C) 2003-2026 Tobias Blomberg / SM0SVX

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
\endverbatim
*/

#ifndef TRUNK_LINK_INCLUDED
#define TRUNK_LINK_INCLUDED


/****************************************************************************
 *
 * System Includes
 *
 ****************************************************************************/

#include <set>
#include <string>
#include <vector>
#include <sigc++/sigc++.h>
#include <json/json.h>


/****************************************************************************
 *
 * Project Includes
 *
 ****************************************************************************/

#include <AsyncConfig.h>
#include <AsyncTcpPrioClient.h>
#include <AsyncFramedTcpConnection.h>
#include <AsyncTimer.h>


/****************************************************************************
 *
 * Forward declarations
 *
 ****************************************************************************/

class Reflector;
class ReflectorMsg;
class MsgUdpAudio;


/****************************************************************************
 *
 * Class definitions
 *
 ****************************************************************************/

/**
@brief  Manages a persistent TCP trunk connection to a peer SvxReflector

One TrunkLink instance is created per [TRUNK_x] config section. It connects
outbound to the peer reflector on the configured port, exchanges a handshake
(MsgTrunkHello), then forwards talker state and audio in both directions for
the set of shared TGs.

Talker arbitration tie-break: each side generates a random 32-bit priority at
startup and exchanges it in MsgTrunkHello. When both sides claim the same TG
simultaneously, the side with the lower priority value defers (clears its local
talker and accepts the remote one).
*/
class TrunkLink : public sigc::trackable
{
  public:
    TrunkLink(Reflector* reflector, Async::Config& cfg,
              const std::string& section);
    ~TrunkLink(void);

    bool initialize(void);

    bool isSharedTG(uint32_t tg) const;
    void setAllPrefixes(const std::vector<std::string>& all_prefixes)
    {
      m_all_prefixes = all_prefixes;
    }
    const std::string& section(void) const { return m_section; }

    Json::Value statusJson(void) const;

    // Called by Reflector when a local client starts/stops on a shared TG
    void onLocalTalkerStart(uint32_t tg, const std::string& callsign);
    void onLocalTalkerStop(uint32_t tg);

    // Called by Reflector for each audio frame from a local talker on a shared TG
    void onLocalAudio(uint32_t tg, const std::vector<uint8_t>& audio);

    // Called by Reflector when a local talker's audio stream ends
    void onLocalFlush(uint32_t tg);

  private:
    static const unsigned HEARTBEAT_TX_CNT_RESET = 10;
    static const unsigned HEARTBEAT_RX_CNT_RESET = 15;

    using FramedTcpClient =
        Async::TcpPrioClient<Async::FramedTcpConnection>;

    Reflector*          m_reflector;
    Async::Config&      m_cfg;
    std::string         m_section;
    std::string         m_peer_host;
    uint16_t            m_peer_port;
    std::string         m_secret;
    std::vector<std::string> m_local_prefix;   // our authoritative TG prefixes
    std::vector<std::string> m_remote_prefix;  // peer's authoritative TG prefixes
    uint32_t            m_priority;       // our tie-break nonce (random)
    uint32_t            m_peer_priority;  // peer's nonce, from MsgTrunkHello
    bool                m_hello_received;
    FramedTcpClient     m_con;
    Async::Timer        m_heartbeat_timer;
    unsigned            m_hb_tx_cnt;
    unsigned            m_hb_rx_cnt;
    std::vector<std::string> m_all_prefixes;   // all prefixes in the mesh
    // TGs where we suppressed our local talker to defer to the peer
    std::set<uint32_t>  m_yielded_tgs;
    // TGs currently held by this specific trunk peer (for scoped cleanup)
    std::set<uint32_t>  m_peer_active_tgs;

    TrunkLink(const TrunkLink&);
    TrunkLink& operator=(const TrunkLink&);

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

};  /* class TrunkLink */


#endif /* TRUNK_LINK_INCLUDED */


/*
 * This file has not been truncated
 */
