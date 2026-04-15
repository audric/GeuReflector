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
#include <ctime>


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
#include "RedisStore.h"
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

  // PAIRED mode: tear down per-host outbound clients
  for (auto* c : m_ob_cons)
  {
    c->disconnect();
    delete c;
  }
  m_ob_cons.clear();

  // PAIRED mode: inbound connections are owned by Reflector's trunk server;
  // just clear our tracking vectors without deleting the objects.
  m_ib_cons.clear();
  m_ib_states.clear();
} /* TrunkLink::~TrunkLink */


bool TrunkLink::initialize(void)
{
  // PAIRED — if set, HOST is a comma-separated list of twin partner hosts
  std::string paired_str;
  m_cfg.getValue(m_section, "PAIRED", paired_str);
  m_paired = (paired_str == "1" || paired_str == "true");

  // HOST
  std::string host_str;
  if (!m_cfg.getValue(m_section, "HOST", host_str) || host_str.empty())
  {
    cerr << "*** ERROR[" << m_section << "]: Missing HOST" << endl;
    return false;
  }

  if (m_paired)
  {
    // Comma-separated host list — one per twin partner.
    std::istringstream iss(host_str);
    std::string h;
    while (std::getline(iss, h, ','))
    {
      h.erase(0, h.find_first_not_of(" \t"));
      h.erase(h.find_last_not_of(" \t") + 1);
      if (!h.empty()) m_peer_hosts.push_back(h);
    }
    if (m_peer_hosts.size() < 2)
    {
      cerr << "*** ERROR[" << m_section
           << "]: PAIRED=1 requires HOST to list at least 2 hosts, got '"
           << host_str << "'" << endl;
      return false;
    }
  }
  else
  {
    m_peer_host = host_str;
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

  // TRUNK_DEBUG — verbose logging for connection diagnostics
  std::string debug_str;
  if (m_cfg.getValue("GLOBAL", "TRUNK_DEBUG", debug_str))
  {
    m_debug = (debug_str == "1" || debug_str == "true" || debug_str == "yes");
  }

  // PEER_ID — name we advertise in our hello (defaults to section name).
  // Used by the receiving peer as the MQTT topic component for this link.
  if (!m_cfg.getValue(m_section, "PEER_ID", m_peer_id_config)
      || m_peer_id_config.empty())
  {
    m_peer_id_config = m_section;
  }

  // BLACKLIST_TGS — flexible filter (exact, prefix "24*", range "10-20")
  std::string blacklist_str;
  if (m_cfg.getValue(m_section, "BLACKLIST_TGS", blacklist_str)
      && !blacklist_str.empty())
  {
    m_blacklist_filter = TgFilter::parse(blacklist_str);
    if (!m_blacklist_filter.empty())
      cout << m_section << ": Blacklisted TGs: "
           << m_blacklist_filter.toString() << endl;
  }

  // ALLOW_TGS — flexible whitelist (empty = allow all)
  std::string allow_str;
  if (m_cfg.getValue(m_section, "ALLOW_TGS", allow_str) && !allow_str.empty())
  {
    m_allow_filter = TgFilter::parse(allow_str);
    if (!m_allow_filter.empty())
      cout << m_section << ": Allowed TGs: "
           << m_allow_filter.toString() << endl;
  }

  // TG_MAP — bidirectional TG remap, syntax "peer:local,peer2:local2"
  std::string tgmap_str;
  if (m_cfg.getValue(m_section, "TG_MAP", tgmap_str) && !tgmap_str.empty())
  {
    std::istringstream ms(tgmap_str);
    std::string pair;
    while (std::getline(ms, pair, ','))
    {
      auto colon = pair.find(':');
      if (colon == std::string::npos) continue;
      try {
        uint32_t peer_tg  = std::stoul(pair.substr(0, colon));
        uint32_t local_tg = std::stoul(pair.substr(colon + 1));
        m_tg_map_in[peer_tg]   = local_tg;
        m_tg_map_out[local_tg] = peer_tg;
      } catch (...) { /* skip malformed */ }
    }
    if (!m_tg_map_in.empty())
    {
      cout << m_section << ": TG mappings (peer<->local):";
      for (const auto& kv : m_tg_map_in)
        cout << " " << kv.first << "<->" << kv.second;
      cout << endl;
    }
  }

  if (m_paired)
  {
    cout << m_section << ": PAIRED=1, " << m_peer_hosts.size()
         << " partner hosts:";
    for (const auto& x : m_peer_hosts) cout << " " << x;
    cout << " port=" << m_peer_port
         << " local_prefix=" << joinPrefixes(m_local_prefix)
         << " remote_prefix=" << joinPrefixes(m_remote_prefix)
         << (m_debug ? " [debug on]" : "")
         << (m_blacklist_filter.empty() ? "" : " [blacklist]")
         << (m_allow_filter.empty()     ? "" : " [whitelist]")
         << (m_tg_map_in.empty()        ? "" : " [tg_map]")
         << endl;

    // Create one outbound client per host
    m_ob_cons.reserve(m_peer_hosts.size());
    m_ob_states.resize(m_peer_hosts.size());
    for (size_t i = 0; i < m_peer_hosts.size(); ++i)
    {
      auto* client = new FramedTcpClient();
      client->setMaxFrameSize(ReflectorMsg::MAX_POSTAUTH_FRAME_SIZE);
      client->connected.connect(
          [this, client]() { onPairedOutboundConnected(client); });
      client->disconnected.connect(
          [this, client](Async::TcpConnection* c,
                         Async::TcpConnection::DisconnectReason r) {
            onPairedOutboundDisconnected(client, c, r);
          });
      client->frameReceived.connect(
          [this, client](Async::FramedTcpConnection* c,
                         std::vector<uint8_t>& data) {
            onPairedOutboundFrame(client, c, data);
          });
      client->addStaticSRVRecord(0, 0, 0, m_peer_port, m_peer_hosts[i]);
      client->setReconnectMinTime(2000);
      client->setReconnectMaxTime(30000);
      client->connect();
      m_ob_cons.push_back(client);

      cout << m_section << ": paired outbound #" << i
           << " connecting to " << m_peer_hosts[i] << ":" << m_peer_port
           << endl;
    }
    return true;
  }

  cout << m_section << ": Trunk to " << m_peer_host << ":" << m_peer_port
       << " local_prefix=" << joinPrefixes(m_local_prefix)
       << " remote_prefix=" << joinPrefixes(m_remote_prefix)
       << (m_debug ? " [debug on]" : "")
       << (m_blacklist_filter.empty() ? "" : " [blacklist]")
       << (m_allow_filter.empty()     ? "" : " [whitelist]")
       << (m_tg_map_in.empty()        ? "" : " [tg_map]")
       << endl;

  m_con.addStaticSRVRecord(0, 0, 0, m_peer_port, m_peer_host);
  m_con.setReconnectMinTime(2000);
  m_con.setReconnectMaxTime(30000);
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


bool TrunkLink::isPeerInterestedTG(uint32_t tg) const
{
  auto it = m_peer_interested_tgs.find(tg);
  if (it == m_peer_interested_tgs.end())
  {
    return false;
  }
  return (std::time(nullptr) - it->second) < PEER_INTEREST_TIMEOUT_S;
} /* TrunkLink::isPeerInterestedTG */


bool TrunkLink::isBlacklisted(uint32_t tg) const
{
  return !m_blacklist_filter.empty() && m_blacklist_filter.matches(tg);
} /* TrunkLink::isBlacklisted */


bool TrunkLink::isAllowed(uint32_t tg) const
{
  return m_allow_filter.empty() || m_allow_filter.matches(tg);
} /* TrunkLink::isAllowed */


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

  // active_talkers: per-peer active TGs (already post-TG_MAP remap).
  // We use m_peer_active_tgs directly instead of filtering the global
  // trunk-talker map by isSharedTG, because a TG_MAP entry can remap a
  // peer-owned wire TG to a local-owned TG that no longer matches the
  // peer's remote prefix — that TG must still appear in this peer's view.
  Json::Value talkers(Json::objectValue);
  for (uint32_t tg : m_peer_active_tgs)
  {
    const std::string cs = TGHandler::instance()->trunkTalkerForTG(tg);
    if (!cs.empty())
    {
      talkers[std::to_string(tg)] = cs;
    }
  }
  obj["active_talkers"] = talkers;

  Json::Value muted_arr(Json::arrayValue);
  for (const auto& cs : m_muted_callsigns) muted_arr.append(cs);
  obj["muted"] = muted_arr;

  return obj;
} /* TrunkLink::statusJson */


void TrunkLink::acceptInboundConnection(Async::FramedTcpConnection* con,
                                         const MsgTrunkHello& hello)
{
  if (m_paired)
  {
    // Paired mode: accept multiple simultaneous inbound connections (D3)
    PairedInboundState st;
    st.hello_received = true;  // Reflector already validated hello
    st.hb_tx_cnt = HEARTBEAT_TX_CNT_RESET;
    st.hb_rx_cnt = HEARTBEAT_RX_CNT_RESET;
    m_ib_cons.push_back(con);
    m_ib_states.push_back(st);

    con->frameReceived.connect(
        [this, con](Async::FramedTcpConnection* /*c*/,
                    std::vector<uint8_t>& data) {
          onPairedInboundFrame(con, data);
        });

    // Use priority/id from whichever inbound completes the handshake first
    // (all twins share same priority; last-writer wins is fine)
    m_peer_priority = hello.priority();
    if (m_peer_id_received.empty())
      m_peer_id_received = hello.id();

    cout << m_section << ": paired inbound accepted from peer '"
         << hello.id() << "' " << con->remoteHost() << ":"
         << con->remotePort() << " priority=" << hello.priority()
         << " total_inbound=" << m_ib_cons.size() << endl;

    m_heartbeat_timer.setEnable(true);

    // Send our hello back on this inbound connection
    sendMsgOnPairedInbound(m_ib_cons.size() - 1,
        MsgTrunkHello(m_peer_id_config, joinPrefixes(m_local_prefix),
                      m_priority, m_secret));
    return;
  }

  // Non-paired mode: only one inbound allowed
  if (m_inbound_con != nullptr)
  {
    cerr << "*** WARNING[" << m_section
         << "]: Already have an inbound connection, rejecting new one" << endl;
    if (m_debug)
    {
      cerr << m_section << " [DEBUG]: existing inbound from "
           << m_inbound_con->remoteHost() << ":"
           << m_inbound_con->remotePort()
           << " ib_hello=" << m_ib_hello_received
           << " ib_hb_rx=" << m_ib_hb_rx_cnt
           << " new inbound from " << con->remoteHost() << ":"
           << con->remotePort() << endl;
    }
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

  m_peer_id_received = hello.id();
  cout << m_section << ": Accepted inbound from " << con->remoteHost()
       << ":" << con->remotePort() << " peer='" << hello.id()
       << "' priority=" << m_peer_priority << endl;
  m_reflector->onTrunkStateChanged(m_section, peerId(), "inbound", true,
                                   con->remoteHost().toString(),
                                   con->remotePort());

  if (m_debug)
  {
    cout << m_section << " [DEBUG]: inbound state: ob_connected="
         << m_con.isConnected() << " ob_hello=" << m_ob_hello_received
         << " ib_hb_tx=" << m_ib_hb_tx_cnt
         << " ib_hb_rx=" << m_ib_hb_rx_cnt << endl;
  }

  // Send our hello back on the inbound connection
  sendMsgOnInbound(MsgTrunkHello(m_peer_id_config,
                                  joinPrefixes(m_local_prefix),
                                  m_priority, m_secret));
} /* TrunkLink::acceptInboundConnection */


void TrunkLink::onInboundDisconnected(Async::FramedTcpConnection* con,
    Async::FramedTcpConnection::DisconnectReason reason)
{
  if (m_paired)
  {
    onPairedInboundDisconnected(con, reason);
    return;
  }

  if (con != m_inbound_con)
  {
    if (m_debug)
    {
      cerr << m_section << " [DEBUG]: onInboundDisconnected for unknown con "
           << con->remoteHost() << ":" << con->remotePort()
           << " (m_inbound_con="
           << (m_inbound_con ? "set" : "null") << ")" << endl;
    }
    return;
  }

  cout << m_section << ": Inbound trunk connection lost" << endl;
  m_reflector->onTrunkStateChanged(m_section, peerId(), "inbound", false);

  if (m_debug)
  {
    cout << m_section << " [DEBUG]: inbound lost: ib_hello=" << m_ib_hello_received
         << " ib_hb_rx=" << m_ib_hb_rx_cnt
         << " ob_connected=" << m_con.isConnected()
         << " ob_hello=" << m_ob_hello_received
         << " peer_active_tgs=" << m_peer_active_tgs.size() << endl;
  }

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
  if (!isActive() || isBlacklisted(tg) || !isAllowed(tg)) return;
  bool mapped = m_tg_map_out.count(tg) > 0;
  if (!mapped &&
      !isSharedTG(tg) && !m_reflector->isClusterTG(tg) &&
      !isPeerInterestedTG(tg))
  {
    return;
  }
  sendMsg(MsgTrunkTalkerStart(mapTgOut(tg), callsign));
} /* TrunkLink::onLocalTalkerStart */


void TrunkLink::onLocalTalkerStop(uint32_t tg)
{
  if (!isActive() || isBlacklisted(tg) || !isAllowed(tg)) return;
  bool mapped = m_tg_map_out.count(tg) > 0;
  if (!mapped &&
      !isSharedTG(tg) && !m_reflector->isClusterTG(tg) &&
      !isPeerInterestedTG(tg))
  {
    return;
  }
  // If we cleared our local talker because we were yielding to this peer,
  // don't send TrunkTalkerStop — the peer already owns the TG.
  if (m_yielded_tgs.count(tg))
  {
    return;
  }
  sendMsg(MsgTrunkTalkerStop(mapTgOut(tg)));
} /* TrunkLink::onLocalTalkerStop */


void TrunkLink::onLocalAudio(uint32_t tg, const std::vector<uint8_t>& audio)
{
  if (!isActive() || isBlacklisted(tg) || !isAllowed(tg)
      || m_yielded_tgs.count(tg))
  {
    return;
  }
  bool mapped = m_tg_map_out.count(tg) > 0;
  if (!mapped &&
      !isSharedTG(tg) && !m_reflector->isClusterTG(tg) &&
      !isPeerInterestedTG(tg))
  {
    return;
  }
  sendMsg(MsgTrunkAudio(mapTgOut(tg), audio));
} /* TrunkLink::onLocalAudio */


void TrunkLink::onLocalFlush(uint32_t tg)
{
  if (!isActive() || isBlacklisted(tg) || !isAllowed(tg)) return;
  bool mapped = m_tg_map_out.count(tg) > 0;
  if (!mapped &&
      !isSharedTG(tg) && !m_reflector->isClusterTG(tg) &&
      !isPeerInterestedTG(tg))
  {
    return;
  }
  sendMsg(MsgTrunkFlush(mapTgOut(tg)));
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
  m_reflector->onTrunkStateChanged(m_section, peerId(), "outbound", true,
                                   m_con.remoteHost().toString(),
                                   m_con.remotePort());

  if (m_debug)
  {
    cout << m_section << " [DEBUG]: outbound up: ib_connected="
         << (m_inbound_con != nullptr) << " ib_hello=" << m_ib_hello_received
         << " sending hello with priority=" << m_priority << endl;
  }

  m_ob_hello_received = false;
  m_ob_hb_tx_cnt = HEARTBEAT_TX_CNT_RESET;
  m_ob_hb_rx_cnt = HEARTBEAT_RX_CNT_RESET;
  m_heartbeat_timer.setEnable(true);

  sendMsgOnOutbound(MsgTrunkHello(m_peer_id_config,
                                   joinPrefixes(m_local_prefix),
                                   m_priority, m_secret));
} /* TrunkLink::onConnected */


void TrunkLink::onDisconnected(TcpConnection* con,
                               TcpConnection::DisconnectReason reason)
{
  cout << m_section << ": Outbound disconnected: "
       << TcpConnection::disconnectReasonStr(reason) << endl;
  m_reflector->onTrunkStateChanged(m_section, peerId(), "outbound", false);

  if (m_debug)
  {
    cout << m_section << " [DEBUG]: outbound lost: ob_hello=" << m_ob_hello_received
         << " ob_hb_rx=" << m_ob_hb_rx_cnt
         << " ib_connected=" << (m_inbound_con != nullptr)
         << " ib_hello=" << m_ib_hello_received << endl;
  }

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

  if (m_debug && header.type() != MsgTrunkHeartbeat::TYPE)
  {
    if (header.type() == MsgTrunkAudio::TYPE ||
        header.type() == MsgTrunkFlush::TYPE)
    {
      if (m_debug_frame_cnt < 500)
      {
        cout << m_section << " [DEBUG]: rx " << (is_inbound ? "IB" : "OB")
             << " type=" << header.type() << " len=" << data.size() << endl;
        if (++m_debug_frame_cnt == 500)
        {
          cout << m_section << " [DEBUG]: audio frame log limit reached (500)"
               << " — suppressing further audio/flush debug" << endl;
        }
      }
    }
    else
    {
      cout << m_section << " [DEBUG]: rx " << (is_inbound ? "IB" : "OB")
           << " type=" << header.type() << " len=" << data.size() << endl;
    }
  }

  switch (header.type())
  {
    case MsgTrunkHeartbeat::TYPE:
      handleMsgTrunkHeartbeat();
      break;
    case MsgTrunkHello::TYPE:
      handleMsgTrunkHello(ss, is_inbound);
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
    case MsgTrunkNodeList::TYPE:
      handleMsgTrunkNodeList(ss);
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


void TrunkLink::handleMsgTrunkHello(std::istream& is, bool is_inbound)
{
  // Inbound hellos are already handled by acceptInboundConnection.
  // A duplicate arriving here means the peer re-sent (e.g. TcpPrioClient
  // background reconnect) — ignore it silently.
  if (is_inbound)
  {
    if (m_debug)
    {
      cout << m_section << " [DEBUG]: ignoring duplicate hello on inbound"
           << endl;
    }
    return;
  }

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
  m_peer_id_received = msg.id();
  m_ob_hello_received = true;

  cout << m_section << ": Trunk hello from peer '" << msg.id()
       << "' local_prefix=" << msg.localPrefix()
       << " priority=" << m_peer_priority
       << " (authenticated)" << endl;

  if (m_debug)
  {
    cout << m_section << " [DEBUG]: hello done: ob_hello=" << m_ob_hello_received
         << " ib_connected=" << (m_inbound_con != nullptr)
         << " ib_hello=" << m_ib_hello_received
         << " isActive=" << isActive() << endl;
  }
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

  uint32_t wire_tg  = msg.tg();
  uint32_t local_tg = mapTgIn(wire_tg);
  if (isBlacklisted(wire_tg) || isBlacklisted(local_tg) || !isAllowed(wire_tg))
  {
    return;
  }
  bool mapped = m_tg_map_in.count(wire_tg) > 0;
  if (!mapped && !isOwnedTG(wire_tg) && !m_reflector->isClusterTG(wire_tg))
  {
    return;
  }

  // Tie-break: if we already have a local talker on this TG, decide who wins.
  // Lower priority value wins. If equal (shouldn't happen), local wins.
  ReflectorClient* local_talker = TGHandler::instance()->talkerForTG(local_tg);
  if (local_talker != nullptr)
  {
    if (m_priority <= m_peer_priority)
    {
      // We win — ignore peer's claim
      cout << m_section << ": TG #" << local_tg
           << " conflict — local wins (our priority=" << m_priority
           << " <= peer=" << m_peer_priority << ")" << endl;
      return;
    }
    // We defer — clear local talker and accept remote
    cout << m_section << ": TG #" << local_tg
         << " conflict — deferring to peer (our priority=" << m_priority
         << " > peer=" << m_peer_priority << ")" << endl;
    m_yielded_tgs.insert(local_tg);
    TGHandler::instance()->setTalkerForTG(local_tg, nullptr);
    // onTalkerUpdated will fire; Reflector must not re-send TrunkTalkerStart
    // for this TG since it's in m_yielded_tgs (checked in Reflector.cpp)
  }

  m_peer_active_tgs.insert(local_tg);
  m_peer_interested_tgs[local_tg] = std::time(nullptr);
  TGHandler::instance()->setTrunkTalkerForTG(local_tg, msg.callsign());
  m_reflector->notifyExternalTrunkTalkerStart(local_tg, m_section, msg.callsign());
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

  uint32_t wire_tg  = msg.tg();
  uint32_t local_tg = mapTgIn(wire_tg);
  if (isBlacklisted(wire_tg) || isBlacklisted(local_tg) || !isAllowed(wire_tg))
  {
    return;
  }
  bool mapped = m_tg_map_in.count(wire_tg) > 0;
  if (!mapped && !isOwnedTG(wire_tg) && !m_reflector->isClusterTG(wire_tg))
  {
    return;
  }

  m_yielded_tgs.erase(local_tg);
  m_peer_active_tgs.erase(local_tg);
  TGHandler::instance()->clearTrunkTalkerForTG(local_tg);
  m_reflector->notifyExternalTrunkTalkerStop(local_tg, m_section);
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

  uint32_t wire_tg  = msg.tg();
  uint32_t local_tg = mapTgIn(wire_tg);
  if (msg.audio().empty()) return;
  if (isBlacklisted(wire_tg) || isBlacklisted(local_tg) || !isAllowed(wire_tg))
  {
    return;
  }
  bool mapped = m_tg_map_in.count(wire_tg) > 0;
  if (!mapped && !isOwnedTG(wire_tg) && !m_reflector->isClusterTG(wire_tg))
  {
    return;
  }

  // Only forward audio if this peer has claimed the TG via TalkerStart
  if (m_peer_active_tgs.find(local_tg) == m_peer_active_tgs.end())
  {
    return;
  }

  // PTY-driven mute: drop audio if the current peer talker is muted
  std::string cs = TGHandler::instance()->trunkTalkerForTG(local_tg);
  if (!cs.empty() && isCallsignMuted(cs))
  {
    return;
  }

  // Refresh peer interest timestamp on audio to keep alive during long TX
  m_peer_interested_tgs[local_tg] = std::time(nullptr);

  // Rebuild a UDP audio message and broadcast to local clients on this TG
  MsgUdpAudio udp_msg(msg.audio());
  m_reflector->broadcastUdpMsg(udp_msg, ReflectorClient::TgFilter(local_tg));

  // Forward trunk audio to connected satellites
  m_reflector->forwardAudioToSatellitesExcept(nullptr, local_tg, msg.audio());
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

  uint32_t wire_tg  = msg.tg();
  uint32_t local_tg = mapTgIn(wire_tg);
  if (isBlacklisted(wire_tg) || isBlacklisted(local_tg) || !isAllowed(wire_tg))
  {
    return;
  }
  bool mapped = m_tg_map_in.count(wire_tg) > 0;
  if (!mapped && !isOwnedTG(wire_tg) && !m_reflector->isClusterTG(wire_tg))
  {
    return;
  }

  m_reflector->broadcastUdpMsg(MsgUdpFlushSamples(),
      ReflectorClient::TgFilter(local_tg));

  // Forward trunk flush to connected satellites
  m_reflector->forwardFlushToSatellitesExcept(nullptr, local_tg);
} /* TrunkLink::handleMsgTrunkFlush */


void TrunkLink::sendMsg(const ReflectorMsg& msg)
{
  if (m_paired)
  {
    // In paired mode, try each outbound client in order (D4 will make sticky)
    for (size_t i = 0; i < m_ob_cons.size(); ++i)
    {
      if (m_ob_cons[i]->isConnected() && m_ob_states[i].hello_received)
      {
        sendMsgOnPairedOutbound(i, msg);
        return;
      }
    }
    // Inbound fallback: try each paired inbound connection
    for (size_t i = 0; i < m_ib_cons.size(); ++i)
    {
      if (m_ib_states[i].hello_received)
      {
        if (m_debug)
        {
          cout << m_section << " [DEBUG]: paired tx fallback to IB#" << i
               << " type=" << msg.type() << endl;
        }
        sendMsgOnPairedInbound(i, msg);
        return;
      }
    }
    if (m_debug)
    {
      cerr << m_section << " [DEBUG]: paired tx dropped type=" << msg.type()
           << " (no active connection)" << endl;
    }
    return;
  }

  if (isOutboundReady())
  {
    sendMsgOnOutbound(msg);
  }
  else if (isInboundReady())
  {
    if (m_debug)
    {
      cout << m_section << " [DEBUG]: tx fallback to IB type="
           << msg.type() << endl;
    }
    sendMsgOnInbound(msg);
  }
  else if (m_debug)
  {
    cerr << m_section << " [DEBUG]: tx dropped type=" << msg.type()
         << " (no active connection)" << endl;
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
  if (m_paired)
  {
    // Per-host outbound heartbeat for paired mode
    bool any_ob_connected = false;
    for (size_t i = 0; i < m_ob_cons.size(); ++i)
    {
      if (!m_ob_cons[i]->isConnected() || m_ob_states[i].hb_rx_cnt == 0)
        continue;
      any_ob_connected = true;
      if (--m_ob_states[i].hb_tx_cnt == 0)
      {
        if (m_debug)
        {
          cout << m_section << " [DEBUG]: paired OB#" << i << " heartbeat tx"
               << " hb_rx=" << m_ob_states[i].hb_rx_cnt << endl;
        }
        sendMsgOnPairedOutbound(i, MsgTrunkHeartbeat());
      }
      if (--m_ob_states[i].hb_rx_cnt == 0)
      {
        cerr << "*** ERROR[" << m_section
             << "]: Paired outbound #" << i << " heartbeat timeout" << endl;
        m_ob_cons[i]->disconnect();
      }
      else if (m_debug && m_ob_states[i].hb_rx_cnt <= 5)
      {
        cerr << m_section << " [DEBUG]: paired OB#" << i
             << " heartbeat rx countdown: " << m_ob_states[i].hb_rx_cnt << endl;
      }
    }

    // Per-host paired inbound heartbeat (D3)
    bool any_ib_connected = false;
    for (size_t i = 0; i < m_ib_cons.size(); ++i)
    {
      if (m_ib_states[i].hb_rx_cnt == 0)
        continue;
      any_ib_connected = true;
      if (--m_ib_states[i].hb_tx_cnt == 0)
      {
        if (m_debug)
        {
          cout << m_section << " [DEBUG]: paired IB#" << i << " heartbeat tx"
               << " hb_rx=" << m_ib_states[i].hb_rx_cnt << endl;
        }
        sendMsgOnPairedInbound(i, MsgTrunkHeartbeat());
      }
      if (--m_ib_states[i].hb_rx_cnt == 0)
      {
        cerr << "*** ERROR[" << m_section
             << "]: Paired inbound #" << i << " heartbeat timeout" << endl;
        m_ib_cons[i]->disconnect();
      }
      else if (m_debug && m_ib_states[i].hb_rx_cnt <= 5)
      {
        cerr << m_section << " [DEBUG]: paired IB#" << i
             << " heartbeat rx countdown: " << m_ib_states[i].hb_rx_cnt << endl;
      }
    }

    // Prune expired peer interest entries
    time_t now = std::time(nullptr);
    for (auto it = m_peer_interested_tgs.begin();
         it != m_peer_interested_tgs.end(); )
    {
      if ((now - it->second) >= PEER_INTEREST_TIMEOUT_S)
        it = m_peer_interested_tgs.erase(it);
      else
        ++it;
    }

    // Disable timer only when all paired outbounds AND all paired inbounds are down
    if (!any_ob_connected && !any_ib_connected)
    {
      m_heartbeat_timer.setEnable(false);
    }
    return;
  }

  // Outbound heartbeat (non-paired)
  if (m_con.isConnected() && m_ob_hb_rx_cnt > 0)
  {
    if (--m_ob_hb_tx_cnt == 0)
    {
      if (m_debug)
      {
        cout << m_section << " [DEBUG]: OB heartbeat tx"
             << " ob_hb_rx=" << m_ob_hb_rx_cnt << endl;
      }
      sendMsgOnOutbound(MsgTrunkHeartbeat());
    }
    if (--m_ob_hb_rx_cnt == 0)
    {
      cerr << "*** ERROR[" << m_section
           << "]: Outbound heartbeat timeout" << endl;
      m_con.disconnect();
    }
    else if (m_debug && m_ob_hb_rx_cnt <= 5)
    {
      cerr << m_section << " [DEBUG]: OB heartbeat rx countdown: "
           << m_ob_hb_rx_cnt << endl;
    }
  }

  // Inbound heartbeat
  if (m_inbound_con != nullptr && m_ib_hb_rx_cnt > 0)
  {
    if (--m_ib_hb_tx_cnt == 0)
    {
      if (m_debug)
      {
        cout << m_section << " [DEBUG]: IB heartbeat tx"
             << " ib_hb_rx=" << m_ib_hb_rx_cnt << endl;
      }
      sendMsgOnInbound(MsgTrunkHeartbeat());
    }
    if (--m_ib_hb_rx_cnt == 0)
    {
      cerr << "*** ERROR[" << m_section
           << "]: Inbound heartbeat timeout" << endl;
      m_inbound_con->disconnect();
    }
    else if (m_debug && m_ib_hb_rx_cnt <= 5)
    {
      cerr << m_section << " [DEBUG]: IB heartbeat rx countdown: "
           << m_ib_hb_rx_cnt << endl;
    }
  }

  // Prune expired peer interest entries
  time_t now = std::time(nullptr);
  for (auto it = m_peer_interested_tgs.begin();
       it != m_peer_interested_tgs.end(); )
  {
    if ((now - it->second) >= PEER_INTEREST_TIMEOUT_S)
    {
      it = m_peer_interested_tgs.erase(it);
    }
    else
    {
      ++it;
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
  if (m_paired)
  {
    // Any paired outbound client that has completed the handshake is enough
    for (size_t i = 0; i < m_ob_cons.size(); ++i)
    {
      if (m_ob_cons[i]->isConnected() && m_ob_states[i].hello_received)
        return true;
    }
    return false;
  }
  return m_con.isConnected() && m_ob_hello_received;
} /* TrunkLink::isOutboundReady */


bool TrunkLink::isInboundReady(void) const
{
  if (m_paired)
  {
    for (size_t i = 0; i < m_ib_cons.size(); ++i)
    {
      if (m_ib_states[i].hello_received)
        return true;
    }
    return false;
  }
  return m_inbound_con != nullptr && m_ib_hello_received;
} /* TrunkLink::isInboundReady */


size_t TrunkLink::pairedClientIndex(FramedTcpClient* client) const
{
  for (size_t i = 0; i < m_ob_cons.size(); ++i)
  {
    if (m_ob_cons[i] == client)
      return i;
  }
  return SIZE_MAX;
} /* TrunkLink::pairedClientIndex */


void TrunkLink::sendMsgOnPairedOutbound(size_t idx, const ReflectorMsg& msg)
{
  if (idx >= m_ob_cons.size()) return;
  ostringstream ss;
  ReflectorMsg header(msg.type());
  if (!header.pack(ss) || !msg.pack(ss))
  {
    cerr << "*** ERROR[" << m_section << "]: Failed to pack trunk message "
            "type=" << msg.type() << " for paired outbound #" << idx << endl;
    return;
  }
  m_ob_states[idx].hb_tx_cnt = HEARTBEAT_TX_CNT_RESET;
  m_ob_cons[idx]->write(ss.str().data(), ss.str().size());
} /* TrunkLink::sendMsgOnPairedOutbound */


size_t TrunkLink::pairedInboundIndex(Async::FramedTcpConnection* con) const
{
  for (size_t i = 0; i < m_ib_cons.size(); ++i)
  {
    if (m_ib_cons[i] == con)
      return i;
  }
  return SIZE_MAX;
} /* TrunkLink::pairedInboundIndex */


void TrunkLink::sendMsgOnPairedInbound(size_t idx, const ReflectorMsg& msg)
{
  if (idx >= m_ib_cons.size()) return;
  ostringstream ss;
  ReflectorMsg header(msg.type());
  if (!header.pack(ss) || !msg.pack(ss))
  {
    cerr << "*** ERROR[" << m_section << "]: Failed to pack trunk message "
            "type=" << msg.type() << " for paired inbound #" << idx << endl;
    return;
  }
  m_ib_states[idx].hb_tx_cnt = HEARTBEAT_TX_CNT_RESET;
  m_ib_cons[idx]->write(ss.str().data(), ss.str().size());
} /* TrunkLink::sendMsgOnPairedInbound */


void TrunkLink::onPairedInboundFrame(Async::FramedTcpConnection* con,
                                      std::vector<uint8_t>& data)
{
  size_t idx = pairedInboundIndex(con);
  if (idx == SIZE_MAX) return;

  // Reset RX heartbeat counter for this inbound connection
  m_ib_states[idx].hb_rx_cnt = HEARTBEAT_RX_CNT_RESET;

  auto buf = reinterpret_cast<const char*>(data.data());
  stringstream ss;
  ss.write(buf, data.size());

  ReflectorMsg header;
  if (!header.unpack(ss))
  {
    cerr << "*** ERROR[" << m_section
         << "]: Failed to unpack frame on paired inbound #" << idx << endl;
    return;
  }

  switch (header.type())
  {
    case MsgTrunkHello::TYPE:
      // Duplicate hello — the hello was already processed by Reflector's
      // acceptInboundConnection path; ignore silently.
      break;

    case MsgTrunkHeartbeat::TYPE:
      // hb_rx_cnt already reset above; nothing else needed
      break;

    default:
      // Dispatch data messages to shared handlers
      switch (header.type())
      {
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
        case MsgTrunkNodeList::TYPE:
          handleMsgTrunkNodeList(ss);
          break;
        default:
          cerr << "*** WARNING[" << m_section
               << "]: Unknown trunk message type=" << header.type()
               << " on paired ib#" << idx << endl;
          break;
      }
      break;
  }
} /* TrunkLink::onPairedInboundFrame */


void TrunkLink::onPairedInboundDisconnected(Async::FramedTcpConnection* con,
    Async::FramedTcpConnection::DisconnectReason /*reason*/)
{
  size_t idx = pairedInboundIndex(con);
  if (idx == SIZE_MAX) return;
  cout << m_section << ": paired inbound #" << idx << " disconnected"
       << " remaining=" << (m_ib_cons.size() - 1) << endl;
  m_ib_cons.erase(m_ib_cons.begin() + idx);
  m_ib_states.erase(m_ib_states.begin() + idx);
  // Reflector owns the connection object — do NOT delete it here.
} /* TrunkLink::onPairedInboundDisconnected */


void TrunkLink::onPairedOutboundConnected(FramedTcpClient* client)
{
  size_t idx = pairedClientIndex(client);
  if (idx == SIZE_MAX) return;

  m_ob_states[idx].hello_received = false;
  m_ob_states[idx].hb_tx_cnt = HEARTBEAT_TX_CNT_RESET;
  m_ob_states[idx].hb_rx_cnt = HEARTBEAT_RX_CNT_RESET;
  m_heartbeat_timer.setEnable(true);

  cout << m_section << ": paired outbound #" << idx
       << " connected to " << m_peer_hosts[idx] << ":" << m_peer_port << endl;

  if (m_debug)
  {
    cout << m_section << " [DEBUG]: paired ob#" << idx
         << " sending hello with priority=" << m_priority << endl;
  }

  sendMsgOnPairedOutbound(idx,
      MsgTrunkHello(m_peer_id_config, joinPrefixes(m_local_prefix),
                    m_priority, m_secret, MsgTrunkHello::ROLE_PEER));
} /* TrunkLink::onPairedOutboundConnected */


void TrunkLink::onPairedOutboundDisconnected(FramedTcpClient* client,
    Async::TcpConnection* /*con*/,
    Async::TcpConnection::DisconnectReason reason)
{
  size_t idx = pairedClientIndex(client);
  if (idx == SIZE_MAX) return;

  cout << m_section << ": paired outbound #" << idx
       << " disconnected ("
       << Async::TcpConnection::disconnectReasonStr(reason) << ")" << endl;

  m_ob_states[idx].hello_received = false;
  m_ob_states[idx].hb_tx_cnt = 0;
  m_ob_states[idx].hb_rx_cnt = 0;

  // TcpPrioClient auto-reconnects; nothing else to do.
} /* TrunkLink::onPairedOutboundDisconnected */


void TrunkLink::onPairedOutboundFrame(FramedTcpClient* client,
    Async::FramedTcpConnection* /*con*/, std::vector<uint8_t>& data)
{
  size_t idx = pairedClientIndex(client);
  if (idx == SIZE_MAX) return;

  // Reset RX heartbeat counter for this client
  m_ob_states[idx].hb_rx_cnt = HEARTBEAT_RX_CNT_RESET;

  auto buf = reinterpret_cast<const char*>(data.data());
  stringstream ss;
  ss.write(buf, data.size());

  ReflectorMsg header;
  if (!header.unpack(ss))
  {
    cerr << "*** ERROR[" << m_section
         << "]: Failed to unpack frame on paired outbound #" << idx << endl;
    return;
  }

  // Only allow hello and heartbeat before handshake completes
  if (!m_ob_states[idx].hello_received &&
      header.type() != MsgTrunkHello::TYPE &&
      header.type() != MsgTrunkHeartbeat::TYPE)
  {
    cerr << "*** WARNING[" << m_section
         << "]: Ignoring paired ob#" << idx << " msg type=" << header.type()
         << " before hello" << endl;
    return;
  }

  switch (header.type())
  {
    case MsgTrunkHello::TYPE:
    {
      MsgTrunkHello msg;
      if (!msg.unpack(ss)) return;

      if (msg.id().empty())
      {
        cerr << "*** ERROR[" << m_section
             << "]: Peer sent empty ID on paired ob#" << idx << endl;
        client->disconnect();
        return;
      }

      if (!msg.verify(m_secret))
      {
        cerr << "*** ERROR[" << m_section
             << "]: HMAC failed on paired outbound #" << idx
             << " (peer='" << msg.id() << "')" << endl;
        client->disconnect();
        return;
      }

      m_ob_states[idx].hello_received = true;
      // Use priority/id from whichever client completes the handshake first
      // (all twins share the same peer priority; last-writer wins is fine here)
      m_peer_priority = msg.priority();
      if (m_peer_id_received.empty())
        m_peer_id_received = msg.id();

      cout << m_section << ": paired outbound #" << idx
           << " hello received (peer='" << msg.id()
           << "' priority=" << msg.priority() << " authenticated)" << endl;
      break;
    }

    case MsgTrunkHeartbeat::TYPE:
      // hb_rx_cnt already reset above; nothing else needed
      break;

    default:
      // For data messages (talker start/stop, audio, flush, node-list),
      // dispatch to the shared handlers — they don't need per-client context.
      // `ss` is already positioned past the header so we can pass it directly.
      switch (header.type())
      {
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
        case MsgTrunkNodeList::TYPE:
          handleMsgTrunkNodeList(ss);
          break;
        default:
          cerr << "*** WARNING[" << m_section
               << "]: Unknown trunk message type=" << header.type()
               << " on paired ob#" << idx << endl;
          break;
      }
      break;
  }
} /* TrunkLink::onPairedOutboundFrame */


void TrunkLink::clearPeerTalkerState(void)
{
  for (uint32_t tg : m_peer_active_tgs)
  {
    TGHandler::instance()->clearTrunkTalkerForTG(tg);
    m_reflector->notifyExternalTrunkTalkerStop(tg, m_section);
  }
  m_peer_active_tgs.clear();
  m_yielded_tgs.clear();
  m_peer_interested_tgs.clear();
} /* TrunkLink::clearPeerTalkerState */


void TrunkLink::reloadConfig(void)
{
  m_blacklist_filter = TgFilter{};
  m_allow_filter     = TgFilter{};
  m_tg_map_in.clear();
  m_tg_map_out.clear();

  RedisStore* rs = m_reflector->redisStore();

  std::string blacklist_str;
  if (rs) {
    blacklist_str = rs->loadTrunkFilter(m_section, "blacklist");
  } else {
    m_cfg.getValue(m_section, "BLACKLIST_TGS", blacklist_str);
  }
  if (!blacklist_str.empty())
  {
    m_blacklist_filter = TgFilter::parse(blacklist_str);
  }

  std::string allow_str;
  if (rs) {
    allow_str = rs->loadTrunkFilter(m_section, "allow");
  } else {
    m_cfg.getValue(m_section, "ALLOW_TGS", allow_str);
  }
  if (!allow_str.empty())
  {
    m_allow_filter = TgFilter::parse(allow_str);
  }

  if (rs) {
    auto tgmap = rs->loadTrunkTgMap(m_section);
    for (auto& kv : tgmap) {
      m_tg_map_in[kv.first]   = kv.second;
      m_tg_map_out[kv.second] = kv.first;
    }
  } else {
    std::string tgmap_str;
    if (m_cfg.getValue(m_section, "TG_MAP", tgmap_str) && !tgmap_str.empty())
    {
      std::istringstream ms(tgmap_str);
      std::string pair;
      while (std::getline(ms, pair, ','))
      {
        auto colon = pair.find(':');
        if (colon == std::string::npos) continue;
        try {
          uint32_t peer_tg  = std::stoul(pair.substr(0, colon));
          uint32_t local_tg = std::stoul(pair.substr(colon + 1));
          m_tg_map_in[peer_tg]   = local_tg;
          m_tg_map_out[local_tg] = peer_tg;
        } catch (...) { /* skip malformed */ }
      }
    }
  }

  cout << m_section << ": Reloaded filters"
       << (m_blacklist_filter.empty() ? "" :
            " blacklist=" + m_blacklist_filter.toString())
       << (m_allow_filter.empty()     ? "" :
            " allow="     + m_allow_filter.toString())
       << " tg_map_entries=" << m_tg_map_in.size()
       << endl;
} /* TrunkLink::reloadConfig */


void TrunkLink::sendNodeList(
    const std::vector<MsgTrunkNodeList::NodeEntry>& nodes)
{
  if (!isActive()) return;
  sendMsg(MsgTrunkNodeList(nodes));
} /* TrunkLink::sendNodeList */


void TrunkLink::handleMsgTrunkNodeList(std::istream& is)
{
  MsgTrunkNodeList msg;
  if (!msg.unpack(is))
  {
    cerr << "*** ERROR[" << m_section
         << "]: Failed to unpack MsgTrunkNodeList" << endl;
    return;
  }
  m_reflector->onPeerNodeList(peerId(), msg.nodes());
} /* TrunkLink::handleMsgTrunkNodeList */


std::string TrunkLink::statusLine(void) const
{
  ostringstream s;
  s << m_section
    << " peer_id=" << peerId()
    << " ob=" << (isOutboundReady() ? "up" : "down")
    << " ib=" << (isInboundReady()  ? "up" : "down")
    << " active_tgs=" << m_peer_active_tgs.size()
    << " muted=" << m_muted_callsigns.size()
    << (m_blacklist_filter.empty() ? "" : " [blacklist]")
    << (m_allow_filter.empty()     ? "" : " [allow]")
    << (m_tg_map_in.empty()        ? "" : " [tg_map]");
  return s.str();
} /* TrunkLink::statusLine */


/*
 * This file has not been truncated
 */
