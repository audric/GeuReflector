/**
@file    TwinLink.cpp
@brief   HA-pair link between two reflectors sharing a LOCAL_PREFIX
@author  GeuReflector
@date    2026-04-15

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


/****************************************************************************
 *
 * System Includes
 *
 ****************************************************************************/

#include <iostream>
#include <sstream>
#include <random>


/****************************************************************************
 *
 * Project Includes
 *
 ****************************************************************************/

#include <AsyncConfig.h>
#include <AsyncTcpConnection.h>


/****************************************************************************
 *
 * Local Includes
 *
 ****************************************************************************/

#include "TwinLink.h"
#include "ReflectorMsg.h"
#include "Reflector.h"
#include "TGHandler.h"
#include <Log.h>


/****************************************************************************
 *
 * Namespaces to use
 *
 ****************************************************************************/

using namespace std;
using namespace Async;
using namespace sigc;


/****************************************************************************
 *
 * TwinLink public methods
 *
 ****************************************************************************/

TwinLink::TwinLink(Reflector* reflector, Async::Config& cfg)
  : m_reflector(reflector), m_cfg(cfg),
    m_peer_port(5304),
    m_peer_id_config("TWIN"),
    m_priority(0), m_peer_priority(0),
    m_inbound_con(nullptr),
    m_heartbeat_timer(1000, Timer::TYPE_PERIODIC, false),
    m_ob_hello_received(false),
    m_ob_hb_tx_cnt(0), m_ob_hb_rx_cnt(0),
    m_ib_hello_received(false),
    m_ib_hb_tx_cnt(0), m_ib_hb_rx_cnt(0)
{
  // Generate a random priority nonce for tie-breaking (once, for lifetime)
  std::random_device rd;
  std::mt19937 rng(rd());
  std::uniform_int_distribution<uint32_t> dist;
  m_priority = dist(rng);

  m_heartbeat_timer.expired.connect(
      mem_fun(*this, &TwinLink::heartbeatTick));

  m_con.connected.connect(mem_fun(*this, &TwinLink::onConnected));
  m_con.disconnected.connect(mem_fun(*this, &TwinLink::onDisconnected));
  m_con.frameReceived.connect(mem_fun(*this, &TwinLink::onFrameReceived));
  m_con.setMaxFrameSize(ReflectorMsg::MAX_POSTAUTH_FRAME_SIZE);
} /* TwinLink::TwinLink */


TwinLink::~TwinLink(void)
{
  m_heartbeat_timer.setEnable(false);
  if (m_inbound_con)
  {
    m_inbound_con->disconnect();
    m_inbound_con = nullptr;
  }
  m_con.disconnect();
} /* TwinLink::~TwinLink */


bool TwinLink::initialize(void)
{
  // HOST
  if (!m_cfg.getValue(m_peer_id_config, "HOST", m_peer_host)
      || m_peer_host.empty())
  {
    cerr << "*** ERROR[TWIN]: Missing HOST in [TWIN] section" << endl;
    return false;
  }

  // PORT (optional, default 5304)
  std::string port_str;
  if (m_cfg.getValue(m_peer_id_config, "PORT", port_str) && !port_str.empty())
  {
    m_peer_port = static_cast<uint16_t>(std::atoi(port_str.c_str()));
  }

  // SECRET
  if (!m_cfg.getValue(m_peer_id_config, "SECRET", m_secret)
      || m_secret.empty())
  {
    cerr << "*** ERROR[TWIN]: Missing SECRET in [TWIN] section" << endl;
    return false;
  }

  // LOCAL_PREFIX — must be present; both partners must share the same value
  if (!m_cfg.getValue("GLOBAL", "LOCAL_PREFIX", m_local_prefix)
      || m_local_prefix.empty())
  {
    cerr << "*** ERROR[TWIN]: Missing LOCAL_PREFIX in [GLOBAL]" << endl;
    return false;
  }

  cout << "TWIN: partner=" << m_peer_host << ":" << m_peer_port
       << " local_prefix=" << m_local_prefix
       << " priority=" << m_priority << endl;

  m_con.addStaticSRVRecord(0, 0, 0, m_peer_port, m_peer_host);
  m_con.setReconnectMinTime(2000);
  m_con.setReconnectMaxTime(30000);
  m_con.connect();

  return true;
} /* TwinLink::initialize */


void TwinLink::acceptInboundConnection(Async::FramedTcpConnection* con,
                                       const MsgTrunkHello& hello)
{
  if (m_inbound_con != nullptr)
  {
    // If the existing inbound's socket is already closed, drop our
    // reference to it and accept the new connection — the old one's
    // disconnect signal may not have fired yet (e.g. partner crashed
    // without a clean FIN).
    if (!m_inbound_con->isConnected())
    {
      geulog::info("twin", "TWIN: replacing stale inbound connection");
      m_inbound_con = nullptr;
      m_ib_hello_received = false;
    }
    else
    {
      geulog::warn("twin", "TWIN: Already have an inbound connection, rejecting new one");
      con->disconnect();
      return;
    }
  }

  m_inbound_con = con;
  m_peer_priority = hello.priority();
  m_ib_hello_received = true;
  m_ib_hb_tx_cnt = 0;
  m_ib_hb_rx_cnt = 0;
  m_heartbeat_timer.setEnable(true);

  // Wire inbound frame handler to our message dispatcher
  con->frameReceived.connect(mem_fun(*this, &TwinLink::onFrameReceived));
  // Wire disconnect handler so we clear m_inbound_con when the remote
  // end goes away, otherwise the next reconnect is rejected above.
  con->disconnected.connect(
      [this](Async::TcpConnection* c,
             Async::TcpConnection::DisconnectReason /*reason*/) {
        if (m_inbound_con == c)
        {
          geulog::info("twin", "TWIN: inbound disconnected");
          m_inbound_con = nullptr;
          m_ib_hello_received = false;
          m_ib_hb_tx_cnt = 0;
          m_ib_hb_rx_cnt = 0;
        }
      });

  m_peer_id_received = hello.id();
  geulog::info("twin", "TWIN: Accepted inbound from ", con->remoteHost(), ":",
      con->remotePort(), " peer='", hello.id(),
      "' priority=", m_peer_priority);

  // Send our hello back on the inbound connection
  sendMsgOnInbound(MsgTrunkHello(m_peer_id_config, m_local_prefix,
                                  m_priority, m_secret,
                                  MsgTrunkHello::ROLE_TWIN));
} /* TwinLink::acceptInboundConnection */


void TwinLink::onLocalTalkerUpdated(uint32_t tg,
                                    const std::string& callsign)
{
  if (!isActive()) return;
  if (callsign.empty())
  {
    sendMsg(MsgTrunkTalkerStop(tg));
  }
  else
  {
    sendMsg(MsgTrunkTalkerStart(tg, callsign));
  }
} /* TwinLink::onLocalTalkerUpdated */


void TwinLink::onLocalAudio(uint32_t tg,
                            const std::vector<uint8_t>& audio)
{
  if (!isActive()) return;
  sendMsg(MsgTrunkAudio(tg, audio));
} /* TwinLink::onLocalAudio */


void TwinLink::onLocalFlush(uint32_t tg)
{
  if (!isActive()) return;
  sendMsg(MsgTrunkFlush(tg));
} /* TwinLink::onLocalFlush */


void TwinLink::onExternalTrunkTalkerStart(uint32_t tg,
                                          const std::string& peer_id,
                                          const std::string& callsign)
{
  if (!isActive()) return;
  sendMsg(MsgTwinExtTalkerStart(tg, peer_id, callsign));
} /* TwinLink::onExternalTrunkTalkerStart */


void TwinLink::onExternalTrunkTalkerStop(uint32_t tg,
                                          const std::string& peer_id)
{
  if (!isActive()) return;
  sendMsg(MsgTwinExtTalkerStop(tg, peer_id));
} /* TwinLink::onExternalTrunkTalkerStop */


bool TwinLink::isActive(void) const
{
  return (m_con.isConnected() && m_ob_hello_received) ||
         (m_inbound_con != nullptr && m_ib_hello_received);
} /* TwinLink::isActive */


/****************************************************************************
 *
 * TwinLink private methods
 *
 ****************************************************************************/

void TwinLink::onConnected(void)
{
  geulog::info("twin", "TWIN: outbound connected to ", m_peer_host, ":", m_peer_port);
  m_ob_hello_received = false;
  m_ob_hb_tx_cnt = 0;
  m_ob_hb_rx_cnt = 0;
  m_heartbeat_timer.setEnable(true);

  sendMsgOnOutbound(MsgTrunkHello(m_peer_id_config, m_local_prefix,
                                   m_priority, m_secret,
                                   MsgTrunkHello::ROLE_TWIN));
} /* TwinLink::onConnected */


void TwinLink::onDisconnected(TcpConnection* /*con*/,
                              TcpConnection::DisconnectReason reason)
{
  geulog::info("twin", "TWIN: outbound disconnected: ",
      TcpConnection::disconnectReasonStr(reason));
  m_ob_hello_received = false;
  m_ob_hb_tx_cnt = 0;
  m_ob_hb_rx_cnt = 0;

  // Disable heartbeat timer if inbound is also down
  if (m_inbound_con == nullptr)
  {
    m_heartbeat_timer.setEnable(false);
  }

  // TcpPrioClient auto-reconnects — nothing else to do
} /* TwinLink::onDisconnected */


void TwinLink::onFrameReceived(FramedTcpConnection* con,
                               std::vector<uint8_t>& data)
{
  bool is_inbound = (con == m_inbound_con);
  bool hello_done = is_inbound ? m_ib_hello_received : m_ob_hello_received;

  auto buf = reinterpret_cast<const char*>(data.data());
  stringstream ss;
  ss.write(buf, data.size());

  ReflectorMsg header;
  if (!header.unpack(ss))
  {
    geulog::error("twin", "TWIN: Failed to unpack frame header");
    return;
  }

  // Only allow hello and heartbeat before hello exchange completes
  if (!hello_done &&
      header.type() != MsgTrunkHello::TYPE &&
      header.type() != MsgTrunkHeartbeat::TYPE)
  {
    geulog::warn("twin", "TWIN: Ignoring message type=", header.type(), " before hello");
    return;
  }

  // Reset RX heartbeat counter for the correct connection
  if (is_inbound)
  {
    m_ib_hb_rx_cnt = 0;
  }
  else
  {
    m_ob_hb_rx_cnt = 0;
  }

  switch (header.type())
  {
    case MsgTrunkHello::TYPE:
      handleMsgTrunkHello(ss, is_inbound);
      break;
    case MsgTrunkHeartbeat::TYPE:
      handleMsgTrunkHeartbeat();
      break;
    case MsgTrunkTalkerStart::TYPE:
      handleMsgTrunkTalkerStart(ss);
      break;
    case MsgTrunkTalkerStop::TYPE:
      handleMsgTrunkTalkerStop(ss);
      break;
    case MsgTrunkAudio::TYPE:
      handleMsgTrunkAudio(ss);
      break;
    case MsgTrunkFlush::TYPE:
      handleMsgTrunkFlush(ss);
      break;
    case MsgTwinExtTalkerStart::TYPE:
      handleMsgTwinExtTalkerStart(ss);
      break;
    case MsgTwinExtTalkerStop::TYPE:
      handleMsgTwinExtTalkerStop(ss);
      break;
    default:
      // Unknown / not-yet-implemented types silently ignored for now
      break;
  }
} /* TwinLink::onFrameReceived */


void TwinLink::handleMsgTrunkHello(std::istream& is, bool is_inbound)
{
  // Inbound hellos are already handled by acceptInboundConnection.
  // A duplicate arriving here means the peer re-sent — ignore it silently.
  if (is_inbound)
  {
    return;
  }

  // Hello on outbound = peer's reply to our outbound hello
  MsgTrunkHello msg;
  if (!msg.unpack(is))
  {
    geulog::error("twin", "TWIN: Failed to unpack MsgTrunkHello");
    return;
  }

  if (msg.role() != MsgTrunkHello::ROLE_TWIN)
  {
    geulog::error("twin", "TWIN: peer sent role=", int(msg.role()),
        " but we expect ROLE_TWIN");
    m_con.disconnect();
    return;
  }

  // Verify shared secret via HMAC
  if (!msg.verify(m_secret))
  {
    geulog::error("twin", "TWIN: HMAC authentication failed — peer '", msg.id(),
        "' sent invalid secret");
    m_con.disconnect();
    return;
  }

  // Both partners must share the same LOCAL_PREFIX
  if (msg.localPrefix() != m_local_prefix)
  {
    geulog::error("twin", "TWIN: local_prefix mismatch: ours='",
        m_local_prefix, "' theirs='", msg.localPrefix(), "'");
    m_con.disconnect();
    return;
  }

  m_peer_priority = msg.priority();
  m_peer_id_received = msg.id();
  m_ob_hello_received = true;

  geulog::info("twin", "TWIN: hello from partner '", msg.id(),
      "' priority=", m_peer_priority, " (authenticated)");
} /* TwinLink::handleMsgTrunkHello */


void TwinLink::handleMsgTrunkTalkerStart(std::istream& is)
{
  MsgTrunkTalkerStart msg;
  if (!msg.unpack(is))
  {
    geulog::error("twin", "TWIN: Failed to unpack MsgTrunkTalkerStart");
    return;
  }
  // setTrunkTalkerForTG fires trunkTalkerUpdated → onTrunkTalkerUpdated in
  // Reflector, which broadcasts MsgTalkerStart to all local clients on this TG.
  TGHandler::instance()->setTrunkTalkerForTG(msg.tg(), msg.callsign());
} /* TwinLink::handleMsgTrunkTalkerStart */


void TwinLink::handleMsgTrunkTalkerStop(std::istream& is)
{
  MsgTrunkTalkerStop msg;
  if (!msg.unpack(is))
  {
    geulog::error("twin", "TWIN: Failed to unpack MsgTrunkTalkerStop");
    return;
  }
  // clearTrunkTalkerForTG fires trunkTalkerUpdated → onTrunkTalkerUpdated,
  // which broadcasts MsgTalkerStop and MsgUdpFlushSamples to local clients.
  TGHandler::instance()->clearTrunkTalkerForTG(msg.tg());
} /* TwinLink::handleMsgTrunkTalkerStop */


void TwinLink::handleMsgTrunkAudio(std::istream& is)
{
  MsgTrunkAudio msg;
  if (!msg.unpack(is))
  {
    geulog::error("twin", "TWIN: Failed to unpack MsgTrunkAudio");
    return;
  }
  if (msg.audio().empty()) return;
  MsgUdpAudio udp_msg(msg.audio());
  m_reflector->broadcastUdpMsg(udp_msg, ReflectorClient::TgFilter(msg.tg()));
  m_reflector->forwardAudioToSatellitesExcept(nullptr, msg.tg(), msg.audio());
} /* TwinLink::handleMsgTrunkAudio */


void TwinLink::handleMsgTrunkFlush(std::istream& is)
{
  MsgTrunkFlush msg;
  if (!msg.unpack(is))
  {
    geulog::error("twin", "TWIN: Failed to unpack MsgTrunkFlush");
    return;
  }
  m_reflector->broadcastUdpMsg(MsgUdpFlushSamples(),
      ReflectorClient::TgFilter(msg.tg()));
  m_reflector->forwardFlushToSatellitesExcept(nullptr, msg.tg());
} /* TwinLink::handleMsgTrunkFlush */


void TwinLink::handleMsgTrunkHeartbeat(void)
{
  // RX counter already reset in onFrameReceived
} /* TwinLink::handleMsgTrunkHeartbeat */


void TwinLink::handleMsgTwinExtTalkerStart(std::istream& is)
{
  MsgTwinExtTalkerStart msg;
  if (!msg.unpack(is))
  {
    geulog::error("twin", "TWIN: Failed to unpack MsgTwinExtTalkerStart");
    return;
  }
  // setTrunkTalkerForTGViaPeer records peer attribution and fires
  // trunkTalkerUpdated → Reflector::onTrunkTalkerUpdated, which broadcasts
  // MsgTalkerStart to local clients.  That handler does NOT call back into
  // TwinLink, so there is no echo loop.
  TGHandler::instance()->setTrunkTalkerForTGViaPeer(
      msg.tg(), msg.callsign(), msg.peerId());
} /* TwinLink::handleMsgTwinExtTalkerStart */


void TwinLink::handleMsgTwinExtTalkerStop(std::istream& is)
{
  MsgTwinExtTalkerStop msg;
  if (!msg.unpack(is)) return;
  // Only clear if this peer still holds the TG — avoids racing a newer talker
  // that took over after the stop was queued on the wire.
  if (TGHandler::instance()->peerIdForTG(msg.tg()) == msg.peerId())
  {
    TGHandler::instance()->setTrunkTalkerForTGViaPeer(
        msg.tg(), "", msg.peerId());
  }
} /* TwinLink::handleMsgTwinExtTalkerStop */


void TwinLink::heartbeatTick(Async::Timer* /*t*/)
{
  // Outbound heartbeat
  if (m_con.isConnected())
  {
    if (++m_ob_hb_tx_cnt >= TWIN_HB_TX_THRESHOLD)
    {
      sendMsgOnOutbound(MsgTrunkHeartbeat());
      m_ob_hb_tx_cnt = 0;
    }
    if (++m_ob_hb_rx_cnt >= TWIN_HB_RX_THRESHOLD)
    {
      geulog::warn("twin", "TWIN: outbound RX timeout, disconnecting");
      m_con.disconnect();
      m_ob_hb_rx_cnt = 0;
    }
  }

  // Inbound heartbeat
  if (m_inbound_con != nullptr && m_inbound_con->isConnected())
  {
    if (++m_ib_hb_tx_cnt >= TWIN_HB_TX_THRESHOLD)
    {
      sendMsgOnInbound(MsgTrunkHeartbeat());
      m_ib_hb_tx_cnt = 0;
    }
    if (++m_ib_hb_rx_cnt >= TWIN_HB_RX_THRESHOLD)
    {
      geulog::warn("twin", "TWIN: inbound RX timeout, disconnecting");
      m_inbound_con->disconnect();
      m_inbound_con = nullptr;
      m_ib_hb_rx_cnt = 0;
      m_ib_hello_received = false;
    }
  }

  // Disable timer when both connections are down
  if (!m_con.isConnected() && m_inbound_con == nullptr)
  {
    m_heartbeat_timer.setEnable(false);
  }
} /* TwinLink::heartbeatTick */


bool TwinLink::sendMsg(const ReflectorMsg& msg)
{
  if (m_con.isConnected() && m_ob_hello_received)
  {
    sendMsgOnOutbound(msg);
    return true;
  }
  if (m_inbound_con != nullptr && m_inbound_con->isConnected()
      && m_ib_hello_received)
  {
    sendMsgOnInbound(msg);
    return true;
  }
  return false;
} /* TwinLink::sendMsg */


void TwinLink::sendMsgOnOutbound(const ReflectorMsg& msg)
{
  if (!m_con.isConnected()) return;
  ostringstream ss;
  ReflectorMsg header(msg.type());
  if (!header.pack(ss) || !msg.pack(ss))
  {
    geulog::error("twin", "TWIN: Failed to pack message type=", msg.type());
    return;
  }
  m_ob_hb_tx_cnt = 0;
  m_con.write(ss.str().data(), ss.str().size());
} /* TwinLink::sendMsgOnOutbound */


void TwinLink::sendMsgOnInbound(const ReflectorMsg& msg)
{
  if (m_inbound_con == nullptr || !m_inbound_con->isConnected()) return;
  ostringstream ss;
  ReflectorMsg header(msg.type());
  if (!header.pack(ss) || !msg.pack(ss))
  {
    geulog::error("twin", "TWIN: Failed to pack message type=", msg.type());
    return;
  }
  m_ib_hb_tx_cnt = 0;
  m_inbound_con->write(ss.str().data(), ss.str().size());
} /* TwinLink::sendMsgOnInbound */


/*
 * This file has not been truncated
 */
