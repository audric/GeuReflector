/**
@file    TrunkLink.cpp
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


/****************************************************************************
 *
 * System Includes
 *
 ****************************************************************************/

#include <iostream>
#include <sstream>
#include <cassert>
#include <random>
#include <cerrno>
#include <cstring>


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

#include "TrunkLink.h"
#include "ReflectorMsg.h"
#include "Reflector.h"
#include "TGHandler.h"
#include "ReflectorClient.h"
#include <json/json.h>


/****************************************************************************
 *
 * Namespaces to use
 *
 ****************************************************************************/

using namespace std;
using namespace Async;
using namespace sigc;


static std::vector<std::string> splitPrefixes(const std::string& s)
{
  std::vector<std::string> result;
  std::istringstream ss(s);
  std::string token;
  while (std::getline(ss, token, ','))
  {
    token.erase(0, token.find_first_not_of(" \t"));
    token.erase(token.find_last_not_of(" \t") + 1);
    if (!token.empty())
      result.push_back(token);
  }
  return result;
}

static std::string joinPrefixes(const std::vector<std::string>& v)
{
  std::string result;
  for (const auto& p : v)
  {
    if (!result.empty()) result += ',';
    result += p;
  }
  return result;
}


/****************************************************************************
 *
 * TrunkLink public methods
 *
 ****************************************************************************/

TrunkLink::TrunkLink(Reflector* reflector, Async::Config& cfg,
                     const std::string& section)
  : m_reflector(reflector), m_cfg(cfg), m_section(section),
    m_peer_port(5302), m_priority(0), m_peer_priority(0),
    m_heartbeat_timer(1000, Timer::TYPE_PERIODIC, false)
{
  // Generate a random priority nonce for tie-breaking (once, for lifetime)
  std::random_device rd;
  std::mt19937 rng(rd());
  std::uniform_int_distribution<uint32_t> dist;
  m_priority = dist(rng);

  m_con.connected.connect(mem_fun(*this, &TrunkLink::onConnected));
  m_con.disconnected.connect(mem_fun(*this, &TrunkLink::onDisconnected));
  m_con.frameReceived.connect(mem_fun(*this, &TrunkLink::onFrameReceived));
  m_con.setMaxFrameSize(ReflectorMsg::MAX_POSTAUTH_FRAME_SIZE);

  m_heartbeat_timer.expired.connect(
      mem_fun(*this, &TrunkLink::heartbeatTick));
} /* TrunkLink::TrunkLink */


TrunkLink::~TrunkLink(void)
{
  // Clear only trunk talkers held by this specific peer
  for (uint32_t tg : m_peer_active_tgs)
  {
    TGHandler::instance()->clearTrunkTalkerForTG(tg);
  }
  m_peer_active_tgs.clear();
} /* TrunkLink::~TrunkLink */


bool TrunkLink::initialize(void)
{
  // HOST
  if (!m_cfg.getValue(m_section, "HOST", m_peer_host) || m_peer_host.empty())
  {
    cerr << "*** ERROR[" << m_section << "]: Missing HOST" << endl;
    return false;
  }

  // PORT (optional, default 5302)
  m_cfg.getValue(m_section, "PORT", m_peer_port);

  // SECRET
  if (!m_cfg.getValue(m_section, "SECRET", m_secret) || m_secret.empty())
  {
    cerr << "*** ERROR[" << m_section << "]: Missing SECRET" << endl;
    return false;
  }

  // LOCAL_PREFIX — comma-separated list of this reflector's owned TG prefixes
  std::string local_prefix_str;
  m_cfg.getValue("GLOBAL", "LOCAL_PREFIX", local_prefix_str);
  m_local_prefix = splitPrefixes(local_prefix_str);
  if (m_local_prefix.empty())
  {
    cerr << "*** ERROR: Missing or empty LOCAL_PREFIX in [GLOBAL]" << endl;
    return false;
  }

  // REMOTE_PREFIX — comma-separated list of the peer's owned TG prefixes
  std::string remote_prefix_str;
  if (!m_cfg.getValue(m_section, "REMOTE_PREFIX", remote_prefix_str) ||
      remote_prefix_str.empty())
  {
    cerr << "*** ERROR[" << m_section << "]: Missing REMOTE_PREFIX" << endl;
    return false;
  }
  m_remote_prefix = splitPrefixes(remote_prefix_str);
  if (m_remote_prefix.empty())
  {
    cerr << "*** ERROR[" << m_section << "]: Invalid REMOTE_PREFIX" << endl;
    return false;
  }

  cout << m_section << ": Trunk to " << m_peer_host << ":" << m_peer_port
       << " local_prefix=" << joinPrefixes(m_local_prefix)
       << " remote_prefix=" << joinPrefixes(m_remote_prefix) << endl;

  m_con.addStaticSRVRecord(0, 0, 0, m_peer_port, m_peer_host);
  m_con.connect();

  return true;
} /* TrunkLink::initialize */


bool TrunkLink::isSharedTG(uint32_t tg) const
{
  const std::string s = std::to_string(tg);

  // Find the best (longest) matching remote prefix for this peer
  size_t best_remote_len = 0;
  for (const auto& prefix : m_remote_prefix)
  {
    if (s.size() >= prefix.size() &&
        s.compare(0, prefix.size(), prefix) == 0 &&
        prefix.size() > best_remote_len)
    {
      best_remote_len = prefix.size();
    }
  }
  if (best_remote_len == 0)
  {
    return false;  // no remote prefix matches at all
  }

  // Check if any prefix in the mesh is a longer match — if so, that other
  // reflector is more specific and this TG doesn't belong to this peer.
  for (const auto& prefix : m_all_prefixes)
  {
    if (prefix.size() > best_remote_len &&
        s.size() >= prefix.size() &&
        s.compare(0, prefix.size(), prefix) == 0)
    {
      return false;  // a longer prefix claims this TG
    }
  }

  return true;
} /* TrunkLink::isSharedTG */


bool TrunkLink::isOwnedTG(uint32_t tg) const
{
  const std::string s = std::to_string(tg);

  // Accept TGs matching our local prefix (TG belongs to us — a peer's
  // client is talking on one of our TGs)
  for (const auto& prefix : m_local_prefix)
  {
    if (s.size() >= prefix.size() &&
        s.compare(0, prefix.size(), prefix) == 0)
    {
      return true;
    }
  }

  // Accept TGs matching the remote prefix (TG belongs to the peer —
  // the peer is reporting its own talker state for our awareness)
  for (const auto& prefix : m_remote_prefix)
  {
    if (s.size() >= prefix.size() &&
        s.compare(0, prefix.size(), prefix) == 0)
    {
      return true;
    }
  }

  return false;
} /* TrunkLink::isOwnedTG */


Json::Value TrunkLink::statusJson(void) const
{
  Json::Value obj(Json::objectValue);
  obj["host"]          = m_peer_host;
  obj["port"]          = m_peer_port;
  obj["connected"]     = isActive();
  obj["outbound_connected"] = m_con.isConnected();
  obj["outbound_hello"]     = m_ob_hello_received;
  obj["inbound_connected"]  = (m_inbound_con != nullptr);
  obj["inbound_hello"]      = m_ib_hello_received;
  Json::Value local_arr(Json::arrayValue);
  for (const auto& p : m_local_prefix)  local_arr.append(p);
  obj["local_prefix"]  = local_arr;

  Json::Value remote_arr(Json::arrayValue);
  for (const auto& p : m_remote_prefix) remote_arr.append(p);
  obj["remote_prefix"] = remote_arr;

  // active_talkers: TGs held by trunk that match remote prefix or are cluster TGs
  Json::Value talkers(Json::objectValue);
  const auto& trunk_map = TGHandler::instance()->trunkTalkersSnapshot();
  for (auto& kv : trunk_map)
  {
    if (isSharedTG(kv.first) || m_reflector->isClusterTG(kv.first))
    {
      talkers[std::to_string(kv.first)] = kv.second;
    }
  }
  obj["active_talkers"] = talkers;

  return obj;
} /* TrunkLink::statusJson */


void TrunkLink::acceptInboundConnection(Async::FramedTcpConnection* con,
                                         const MsgTrunkHello& hello)
{
  if (m_inbound_con != nullptr)
  {
    cerr << "*** WARNING[" << m_section
         << "]: Already have an inbound connection, rejecting new one" << endl;
    con->disconnect();
    return;
  }

  m_inbound_con = con;
  m_peer_priority = hello.priority();
  m_ib_hello_received = true;

  m_ib_hb_tx_cnt = HEARTBEAT_TX_CNT_RESET;
  m_ib_hb_rx_cnt = HEARTBEAT_RX_CNT_RESET;
  m_heartbeat_timer.setEnable(true);
  m_yielded_tgs.clear();

  // Wire inbound frame handler to our message dispatcher
  con->frameReceived.connect(
      mem_fun(*this, &TrunkLink::onFrameReceived));

  cout << m_section << ": Accepted inbound from " << con->remoteHost()
       << ":" << con->remotePort() << " peer='" << hello.id()
       << "' priority=" << m_peer_priority << endl;

  // Send our hello back on the inbound connection
  sendMsgOnInbound(MsgTrunkHello(m_section, joinPrefixes(m_local_prefix),
                                  m_priority, m_secret));
} /* TrunkLink::acceptInboundConnection */


void TrunkLink::onInboundDisconnected(Async::FramedTcpConnection* con,
    Async::FramedTcpConnection::DisconnectReason reason)
{
  if (con != m_inbound_con)
  {
    return;
  }

  cout << m_section << ": Inbound trunk connection lost" << endl;

  m_inbound_con = nullptr;
  m_ib_hello_received = false;
  m_ib_hb_tx_cnt = 0;
  m_ib_hb_rx_cnt = 0;

  // Peer's data channel is gone — clear peer talker state
  clearPeerTalkerState();

  // Disable heartbeat timer if outbound is also down
  if (!m_con.isConnected())
  {
    m_heartbeat_timer.setEnable(false);
  }
} /* TrunkLink::onInboundDisconnected */


void TrunkLink::onLocalTalkerStart(uint32_t tg, const std::string& callsign)
{
  if (!isActive() ||
      (!isSharedTG(tg) && !m_reflector->isClusterTG(tg)))
  {
    return;
  }
  sendMsg(MsgTrunkTalkerStart(tg, callsign));
} /* TrunkLink::onLocalTalkerStart */


void TrunkLink::onLocalTalkerStop(uint32_t tg)
{
  if (!isActive() ||
      (!isSharedTG(tg) && !m_reflector->isClusterTG(tg)))
  {
    return;
  }
  // If we cleared our local talker because we were yielding to this peer,
  // don't send TrunkTalkerStop — the peer already owns the TG.
  if (m_yielded_tgs.count(tg))
  {
    return;
  }
  sendMsg(MsgTrunkTalkerStop(tg));
} /* TrunkLink::onLocalTalkerStop */


void TrunkLink::onLocalAudio(uint32_t tg, const std::vector<uint8_t>& audio)
{
  if (!isActive() ||
      (!isSharedTG(tg) && !m_reflector->isClusterTG(tg)) ||
      m_yielded_tgs.count(tg))
  {
    return;
  }
  sendMsg(MsgTrunkAudio(tg, audio));
} /* TrunkLink::onLocalAudio */


void TrunkLink::onLocalFlush(uint32_t tg)
{
  if (!isActive() ||
      (!isSharedTG(tg) && !m_reflector->isClusterTG(tg)))
  {
    return;
  }
  sendMsg(MsgTrunkFlush(tg));
} /* TrunkLink::onLocalFlush */


/****************************************************************************
 *
 * TrunkLink private methods
 *
 ****************************************************************************/

void TrunkLink::onConnected(void)
{
  cout << m_section << ": Outbound connected to " << m_con.remoteHost()
       << ":" << m_con.remotePort() << endl;

  m_ob_hello_received = false;
  m_ob_hb_tx_cnt = HEARTBEAT_TX_CNT_RESET;
  m_ob_hb_rx_cnt = HEARTBEAT_RX_CNT_RESET;
  m_heartbeat_timer.setEnable(true);

  sendMsgOnOutbound(MsgTrunkHello(m_section, joinPrefixes(m_local_prefix),
                                   m_priority, m_secret));
} /* TrunkLink::onConnected */


void TrunkLink::onDisconnected(TcpConnection* con,
                               TcpConnection::DisconnectReason reason)
{
  cout << m_section << ": Outbound disconnected: "
       << TcpConnection::disconnectReasonStr(reason) << endl;

  m_ob_hello_received = false;
  m_ob_hb_tx_cnt = 0;
  m_ob_hb_rx_cnt = 0;

  // Disable heartbeat timer if inbound is also down
  if (m_inbound_con == nullptr)
  {
    m_heartbeat_timer.setEnable(false);
  }

  // TcpPrioClient auto-reconnects — nothing else to do
} /* TrunkLink::onDisconnected */


void TrunkLink::onFrameReceived(FramedTcpConnection* con,
                                std::vector<uint8_t>& data)
{
  auto buf = reinterpret_cast<const char*>(data.data());
  stringstream ss;
  ss.write(buf, data.size());

  ReflectorMsg header;
  if (!header.unpack(ss))
  {
    cerr << "*** ERROR[" << m_section << "]: Failed to unpack trunk message "
            "header" << endl;
    return;
  }

  // Determine which connection this frame arrived on
  bool is_inbound = (con == m_inbound_con);
  bool hello_done = is_inbound ? m_ib_hello_received : m_ob_hello_received;

  // Only allow hello and heartbeat before hello exchange completes
  if (!hello_done &&
      header.type() != MsgTrunkHello::TYPE &&
      header.type() != MsgTrunkHeartbeat::TYPE)
  {
    cerr << "*** WARNING[" << m_section
         << "]: Ignoring trunk message type=" << header.type()
         << " before hello" << endl;
    return;
  }

  // Reset RX counter for the correct connection
  if (is_inbound)
  {
    m_ib_hb_rx_cnt = HEARTBEAT_RX_CNT_RESET;
  }
  else
  {
    m_ob_hb_rx_cnt = HEARTBEAT_RX_CNT_RESET;
  }

  switch (header.type())
  {
    case MsgTrunkHeartbeat::TYPE:
      handleMsgTrunkHeartbeat();
      break;
    case MsgTrunkHello::TYPE:
      handleMsgTrunkHello(ss);
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
    default:
      cerr << "*** WARNING[" << m_section
           << "]: Unknown trunk message type=" << header.type() << endl;
      break;
  }
} /* TrunkLink::onFrameReceived */


void TrunkLink::handleMsgTrunkHeartbeat(void)
{
  // rx counter already reset in onFrameReceived
} /* TrunkLink::handleMsgTrunkHeartbeat */


void TrunkLink::handleMsgTrunkHello(std::istream& is)
{
  // Hello on outbound = peer's reply to our outbound hello
  MsgTrunkHello msg;
  if (!msg.unpack(is))
  {
    cerr << "*** ERROR[" << m_section << "]: Failed to unpack MsgTrunkHello"
         << endl;
    return;
  }

  if (msg.id().empty())
  {
    cerr << "*** ERROR[" << m_section
         << "]: Peer sent empty trunk ID in MsgTrunkHello" << endl;
    m_con.disconnect();
    return;
  }

  // Verify shared secret via HMAC
  if (!msg.verify(m_secret))
  {
    cerr << "*** ERROR[" << m_section
         << "]: Trunk authentication failed — peer '" << msg.id()
         << "' sent invalid secret (HMAC mismatch)" << endl;
    m_con.disconnect();
    return;
  }

  m_peer_priority = msg.priority();
  m_ob_hello_received = true;

  cout << m_section << ": Trunk hello from peer '" << msg.id()
       << "' local_prefix=" << msg.localPrefix()
       << " priority=" << m_peer_priority
       << " (authenticated)" << endl;
} /* TrunkLink::handleMsgTrunkHello */


void TrunkLink::handleMsgTrunkTalkerStart(std::istream& is)
{
  MsgTrunkTalkerStart msg;
  if (!msg.unpack(is))
  {
    cerr << "*** ERROR[" << m_section
         << "]: Failed to unpack MsgTrunkTalkerStart" << endl;
    return;
  }

  uint32_t tg = msg.tg();
  if (!isOwnedTG(tg) && !m_reflector->isClusterTG(tg))
  {
    return;
  }

  // Tie-break: if we already have a local talker on this TG, decide who wins.
  // Lower priority value wins. If equal (shouldn't happen), local wins.
  ReflectorClient* local_talker = TGHandler::instance()->talkerForTG(tg);
  if (local_talker != nullptr)
  {
    if (m_priority <= m_peer_priority)
    {
      // We win — ignore peer's claim
      cout << m_section << ": TG #" << tg
           << " conflict — local wins (our priority=" << m_priority
           << " <= peer=" << m_peer_priority << ")" << endl;
      return;
    }
    // We defer — clear local talker and accept remote
    cout << m_section << ": TG #" << tg
         << " conflict — deferring to peer (our priority=" << m_priority
         << " > peer=" << m_peer_priority << ")" << endl;
    m_yielded_tgs.insert(tg);
    TGHandler::instance()->setTalkerForTG(tg, nullptr);
    // onTalkerUpdated will fire; Reflector must not re-send TrunkTalkerStart
    // for this TG since it's in m_yielded_tgs (checked in Reflector.cpp)
  }

  m_peer_active_tgs.insert(tg);
  TGHandler::instance()->setTrunkTalkerForTG(tg, msg.callsign());
} /* TrunkLink::handleMsgTrunkTalkerStart */


void TrunkLink::handleMsgTrunkTalkerStop(std::istream& is)
{
  MsgTrunkTalkerStop msg;
  if (!msg.unpack(is))
  {
    cerr << "*** ERROR[" << m_section
         << "]: Failed to unpack MsgTrunkTalkerStop" << endl;
    return;
  }

  uint32_t tg = msg.tg();
  if (!isOwnedTG(tg) && !m_reflector->isClusterTG(tg))
  {
    return;
  }

  m_yielded_tgs.erase(tg);
  m_peer_active_tgs.erase(tg);
  TGHandler::instance()->clearTrunkTalkerForTG(tg);
} /* TrunkLink::handleMsgTrunkTalkerStop */


void TrunkLink::handleMsgTrunkAudio(std::istream& is)
{
  MsgTrunkAudio msg;
  if (!msg.unpack(is))
  {
    cerr << "*** ERROR[" << m_section
         << "]: Failed to unpack MsgTrunkAudio" << endl;
    return;
  }

  uint32_t tg = msg.tg();
  if ((!isOwnedTG(tg) && !m_reflector->isClusterTG(tg)) || msg.audio().empty())
  {
    return;
  }

  // Only forward audio if this peer has claimed the TG via TalkerStart
  if (m_peer_active_tgs.find(tg) == m_peer_active_tgs.end())
  {
    return;
  }

  // Rebuild a UDP audio message and broadcast to local clients on this TG
  MsgUdpAudio udp_msg(msg.audio());
  m_reflector->broadcastUdpMsg(udp_msg, ReflectorClient::TgFilter(tg));

  // Forward trunk audio to connected satellites
  m_reflector->forwardAudioToSatellitesExcept(nullptr, tg, msg.audio());
} /* TrunkLink::handleMsgTrunkAudio */


void TrunkLink::handleMsgTrunkFlush(std::istream& is)
{
  MsgTrunkFlush msg;
  if (!msg.unpack(is))
  {
    cerr << "*** ERROR[" << m_section
         << "]: Failed to unpack MsgTrunkFlush" << endl;
    return;
  }

  uint32_t tg = msg.tg();
  if (!isOwnedTG(tg) && !m_reflector->isClusterTG(tg))
  {
    return;
  }

  m_reflector->broadcastUdpMsg(MsgUdpFlushSamples(),
      ReflectorClient::TgFilter(tg));

  // Forward trunk flush to connected satellites
  m_reflector->forwardFlushToSatellitesExcept(nullptr, tg);
} /* TrunkLink::handleMsgTrunkFlush */


void TrunkLink::sendMsg(const ReflectorMsg& msg)
{
  if (isOutboundReady())
  {
    sendMsgOnOutbound(msg);
  }
  else if (isInboundReady())
  {
    sendMsgOnInbound(msg);
  }
} /* TrunkLink::sendMsg */


void TrunkLink::sendMsgOnOutbound(const ReflectorMsg& msg)
{
  ostringstream ss;
  ReflectorMsg header(msg.type());
  if (!header.pack(ss) || !msg.pack(ss))
  {
    cerr << "*** ERROR[" << m_section << "]: Failed to pack trunk message "
            "type=" << msg.type() << endl;
    return;
  }
  m_ob_hb_tx_cnt = HEARTBEAT_TX_CNT_RESET;
  m_con.write(ss.str().data(), ss.str().size());
} /* TrunkLink::sendMsgOnOutbound */


void TrunkLink::sendMsgOnInbound(const ReflectorMsg& msg)
{
  if (m_inbound_con == nullptr) return;
  ostringstream ss;
  ReflectorMsg header(msg.type());
  if (!header.pack(ss) || !msg.pack(ss))
  {
    cerr << "*** ERROR[" << m_section << "]: Failed to pack trunk message "
            "type=" << msg.type() << endl;
    return;
  }
  m_ib_hb_tx_cnt = HEARTBEAT_TX_CNT_RESET;
  m_inbound_con->write(ss.str().data(), ss.str().size());
} /* TrunkLink::sendMsgOnInbound */


void TrunkLink::heartbeatTick(Async::Timer* t)
{
  // Outbound heartbeat
  if (m_con.isConnected() && m_ob_hb_rx_cnt > 0)
  {
    if (--m_ob_hb_tx_cnt == 0)
    {
      sendMsgOnOutbound(MsgTrunkHeartbeat());
    }
    if (--m_ob_hb_rx_cnt == 0)
    {
      cerr << "*** ERROR[" << m_section
           << "]: Outbound heartbeat timeout" << endl;
      m_con.disconnect();
    }
  }

  // Inbound heartbeat
  if (m_inbound_con != nullptr && m_ib_hb_rx_cnt > 0)
  {
    if (--m_ib_hb_tx_cnt == 0)
    {
      sendMsgOnInbound(MsgTrunkHeartbeat());
    }
    if (--m_ib_hb_rx_cnt == 0)
    {
      cerr << "*** ERROR[" << m_section
           << "]: Inbound heartbeat timeout" << endl;
      m_inbound_con->disconnect();
    }
  }

  // Disable timer when both connections are down
  if (!m_con.isConnected() && m_inbound_con == nullptr)
  {
    m_heartbeat_timer.setEnable(false);
  }
} /* TrunkLink::heartbeatTick */


bool TrunkLink::isActive(void) const
{
  return isOutboundReady() || isInboundReady();
} /* TrunkLink::isActive */


bool TrunkLink::isOutboundReady(void) const
{
  return m_con.isConnected() && m_ob_hello_received;
} /* TrunkLink::isOutboundReady */


bool TrunkLink::isInboundReady(void) const
{
  return m_inbound_con != nullptr && m_ib_hello_received;
} /* TrunkLink::isInboundReady */


void TrunkLink::clearPeerTalkerState(void)
{
  for (uint32_t tg : m_peer_active_tgs)
  {
    TGHandler::instance()->clearTrunkTalkerForTG(tg);
  }
  m_peer_active_tgs.clear();
  m_yielded_tgs.clear();
} /* TrunkLink::clearPeerTalkerState */


/*
 * This file has not been truncated
 */
