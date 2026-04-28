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
#include <cmath>
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
#include <Log.h>


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


// Strip control chars (< 0x20 and 0x7f) and ':' (Redis key delimiter),
// then truncate to `max_len`. Used for untrusted identifiers received
// over the trunk: peer_id from hellos and callsigns from node lists.
static std::string sanitizeIdent(const std::string& in, size_t max_len)
{
  std::string out;
  out.reserve(std::min(in.size(), max_len));
  for (char c : in)
  {
    unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f) continue;
    if (c == ':') continue;
    out += c;
    if (out.size() >= max_len) break;
  }
  return out;
}

// Strip control chars only and truncate to `max_bytes`. Used for free-
// form text fields (e.g. QTH name) where non-ASCII bytes (UTF-8) are
// legitimate. Truncation is byte-level; callers tolerate the possibility
// of a split UTF-8 sequence at the boundary.
static std::string sanitizeText(const std::string& in, size_t max_bytes)
{
  std::string out;
  out.reserve(std::min(in.size(), max_bytes));
  for (char c : in)
  {
    unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f) continue;
    out += c;
    if (out.size() >= max_bytes) break;
  }
  return out;
}

// Recursively sanitize string values inside an untrusted JSON tree
// received from a peer. Caps depth, container size, and per-string
// length. Mutates in place.
static void sanitizeJsonStrings(Json::Value& v, unsigned depth = 0)
{
  static constexpr unsigned MAX_DEPTH       = 8;
  static constexpr Json::ArrayIndex MAX_LEN = 256;
  static constexpr size_t MAX_STR_BYTES     = 1024;
  if (depth >= MAX_DEPTH)
  {
    v = Json::Value();  // null out the subtree
    return;
  }
  if (v.isString())
  {
    v = sanitizeText(v.asString(), MAX_STR_BYTES);
  }
  else if (v.isArray())
  {
    if (v.size() > MAX_LEN) v.resize(MAX_LEN);
    for (Json::ArrayIndex i = 0; i < v.size(); ++i)
    {
      sanitizeJsonStrings(v[i], depth + 1);
    }
  }
  else if (v.isObject())
  {
    auto names = v.getMemberNames();
    if (names.size() > MAX_LEN)
    {
      // Trim deterministically by removing trailing keys.
      for (size_t i = MAX_LEN; i < names.size(); ++i)
      {
        v.removeMember(names[i]);
      }
      names.resize(MAX_LEN);
    }
    for (const auto& k : names)
    {
      sanitizeJsonStrings(v[k], depth + 1);
    }
  }
}

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
    geulog::error("trunk", "[", m_section, "] Missing HOST");
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
      geulog::error("trunk", "[", m_section,
                    "] PAIRED=1 requires HOST to list at least 2 hosts, got '",
                    host_str, "'");
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
    geulog::error("trunk", "[", m_section, "] Missing SECRET");
    return false;
  }

  // LOCAL_PREFIX — comma-separated list of this reflector's owned TG prefixes
  std::string local_prefix_str;
  m_cfg.getValue("GLOBAL", "LOCAL_PREFIX", local_prefix_str);
  m_local_prefix = splitPrefixes(local_prefix_str);
  if (m_local_prefix.empty())
  {
    geulog::error("trunk", "Missing or empty LOCAL_PREFIX in [GLOBAL]");
    return false;
  }

  // REMOTE_PREFIX — comma-separated list of the peer's owned TG prefixes
  std::string remote_prefix_str;
  if (!m_cfg.getValue(m_section, "REMOTE_PREFIX", remote_prefix_str) ||
      remote_prefix_str.empty())
  {
    geulog::error("trunk", "[", m_section, "] Missing REMOTE_PREFIX");
    return false;
  }
  m_remote_prefix = splitPrefixes(remote_prefix_str);
  if (m_remote_prefix.empty())
  {
    geulog::error("trunk", "[", m_section, "] Invalid REMOTE_PREFIX");
    return false;
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
      geulog::info("trunk", m_section, ": Blacklisted TGs: ",
                   m_blacklist_filter.toString());
  }

  // ALLOW_TGS — flexible whitelist (empty = allow all)
  std::string allow_str;
  if (m_cfg.getValue(m_section, "ALLOW_TGS", allow_str) && !allow_str.empty())
  {
    m_allow_filter = TgFilter::parse(allow_str);
    if (!m_allow_filter.empty())
      geulog::info("trunk", m_section, ": Allowed TGs: ",
                   m_allow_filter.toString());
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
      std::ostringstream _tgmap_oss;
      _tgmap_oss << m_section << ": TG mappings (peer<->local):";
      for (const auto& kv : m_tg_map_in)
        _tgmap_oss << " " << kv.first << "<->" << kv.second;
      geulog::info("trunk", _tgmap_oss.str());
    }
  }

  if (m_paired)
  {
    {
      std::ostringstream _paired_oss;
      _paired_oss << m_section << ": PAIRED=1, " << m_peer_hosts.size()
                  << " partner hosts:";
      for (const auto& x : m_peer_hosts) _paired_oss << " " << x;
      _paired_oss << " port=" << m_peer_port
                  << " local_prefix=" << joinPrefixes(m_local_prefix)
                  << " remote_prefix=" << joinPrefixes(m_remote_prefix)
                  << (m_blacklist_filter.empty() ? "" : " [blacklist]")
                  << (m_allow_filter.empty()     ? "" : " [whitelist]")
                  << (m_tg_map_in.empty()        ? "" : " [tg_map]");
      geulog::info("trunk", _paired_oss.str());
    }

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

      geulog::info("trunk", m_section, ": paired outbound #", i,
                   " connecting to ", m_peer_hosts[i], ":", m_peer_port);
    }
    return true;
  }

  geulog::info("trunk", m_section, ": Trunk to ", m_peer_host, ":", m_peer_port,
               " local_prefix=", joinPrefixes(m_local_prefix),
               " remote_prefix=", joinPrefixes(m_remote_prefix),
               (m_blacklist_filter.empty() ? "" : " [blacklist]"),
               (m_allow_filter.empty()     ? "" : " [whitelist]"),
               (m_tg_map_in.empty()        ? "" : " [tg_map]"));

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

  Json::Value nodes_arr(Json::arrayValue);
  for (const auto& n : m_partner_nodes)
  {
    // Start from the rich blob if present; fall back to the flat fields
    // when the peer didn't supply one.
    Json::Value entry = n.status.isObject() ? n.status
                                            : Json::Value(Json::objectValue);
    entry["callsign"] = n.callsign;
    entry["tg"]       = static_cast<Json::UInt>(n.tg);
    if (n.lat != 0.0f || n.lon != 0.0f)
    {
      entry["lat"] = n.lat;
      entry["lon"] = n.lon;
    }
    if (!n.qth_name.empty()) entry["qth_name"] = n.qth_name;
    if (!n.sat_id.empty())   entry["sat_id"]   = n.sat_id;
    // Derive isTalker from the live trunk-talker map maintained by
    // MsgPeerTalkerStart/Stop. Authoritative for partner nodes; the
    // sender's own isTalker is stale by the time it arrives in the
    // periodic node-list push.
    entry["isTalker"] =
        (TGHandler::instance()->trunkTalkerForTG(n.tg) == n.callsign);
    nodes_arr.append(entry);
  }
  obj["nodes"] = nodes_arr;

  return obj;
} /* TrunkLink::statusJson */


void TrunkLink::acceptInboundConnection(Async::FramedTcpConnection* con,
                                         const MsgPeerHello& hello)
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
      m_peer_id_received = sanitizeIdent(hello.id(), 64);

    geulog::info("trunk", m_section, ": paired inbound accepted from peer '",
                 hello.id(), "' ", con->remoteHost(), ":",
                 con->remotePort(), " priority=", hello.priority(),
                 " total_inbound=", m_ib_cons.size());

    m_heartbeat_timer.setEnable(true);

    // Send our hello back on this inbound connection
    sendMsgOnPairedInbound(m_ib_cons.size() - 1,
        MsgPeerHello(m_peer_id_config, joinPrefixes(m_local_prefix),
                      m_priority, m_secret));
    return;
  }

  // Non-paired mode: only one inbound allowed
  if (m_inbound_con != nullptr)
  {
    geulog::warn("trunk", "[", m_section,
                 "]: Already have an inbound connection, rejecting new one");
    geulog::debug("trunk", m_section, ": existing inbound from ",
                  m_inbound_con->remoteHost(), ":",
                  m_inbound_con->remotePort(),
                  " ib_hello=", m_ib_hello_received,
                  " ib_hb_rx=", m_ib_hb_rx_cnt,
                  " new inbound from ", con->remoteHost(), ":",
                  con->remotePort());
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

  m_peer_id_received = sanitizeIdent(hello.id(), 64);
  geulog::info("trunk", m_section, ": Accepted inbound from ",
               con->remoteHost(), ":", con->remotePort(),
               " peer='", hello.id(), "' priority=", m_peer_priority);
  m_reflector->onTrunkStateChanged(m_section, peerId(), "inbound", true,
                                   con->remoteHost().toString(),
                                   con->remotePort());

  geulog::debug("trunk", m_section, ": inbound state: ob_connected=",
                m_con.isConnected(), " ob_hello=", m_ob_hello_received,
                " ib_hb_tx=", m_ib_hb_tx_cnt, " ib_hb_rx=", m_ib_hb_rx_cnt);

  // Send our hello back on the inbound connection
  sendMsgOnInbound(MsgPeerHello(m_peer_id_config,
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
    geulog::debug("trunk", m_section, ": onInboundDisconnected for unknown con ",
                  con->remoteHost(), ":", con->remotePort(),
                  " (m_inbound_con=", (m_inbound_con ? "set" : "null"), ")");
    return;
  }

  geulog::info("trunk", m_section, ": Inbound trunk connection lost");
  geulog::debug("trunk", m_section, ": inbound lost: ib_hello=",
                m_ib_hello_received, " ib_hb_rx=", m_ib_hb_rx_cnt,
                " ob_connected=", m_con.isConnected(),
                " ob_hello=", m_ob_hello_received,
                " peer_active_tgs=", m_peer_active_tgs.size());

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

  // Emit after state is cleared so isActive()-based cleanup in consumers
  // (e.g. Redis peer-node mirror) sees the correct post-disconnect state.
  m_reflector->onTrunkStateChanged(m_section, peerId(), "inbound", false);

  if (!isActive()) m_partner_nodes.clear();
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
  sendMsg(MsgPeerTalkerStart(mapTgOut(tg), callsign));
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
  sendMsg(MsgPeerTalkerStop(mapTgOut(tg)));
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
  sendMsg(MsgPeerAudio(mapTgOut(tg), audio));
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
  sendMsg(MsgPeerFlush(mapTgOut(tg)));
} /* TrunkLink::onLocalFlush */


/****************************************************************************
 *
 * TrunkLink private methods
 *
 ****************************************************************************/

void TrunkLink::onConnected(void)
{
  geulog::info("trunk", m_section, ": Outbound connected to ",
               m_con.remoteHost(), ":", m_con.remotePort());
  m_reflector->onTrunkStateChanged(m_section, peerId(), "outbound", true,
                                   m_con.remoteHost().toString(),
                                   m_con.remotePort());

  geulog::debug("trunk", m_section, ": outbound up: ib_connected=",
                (m_inbound_con != nullptr), " ib_hello=", m_ib_hello_received,
                " sending hello with priority=", m_priority);

  m_ob_hello_received = false;
  m_ob_hb_tx_cnt = HEARTBEAT_TX_CNT_RESET;
  m_ob_hb_rx_cnt = HEARTBEAT_RX_CNT_RESET;
  m_heartbeat_timer.setEnable(true);

  sendMsgOnOutbound(MsgPeerHello(m_peer_id_config,
                                   joinPrefixes(m_local_prefix),
                                   m_priority, m_secret));
} /* TrunkLink::onConnected */


void TrunkLink::onDisconnected(TcpConnection* con,
                               TcpConnection::DisconnectReason reason)
{
  geulog::info("trunk", m_section, ": Outbound disconnected: ",
               TcpConnection::disconnectReasonStr(reason));
  geulog::debug("trunk", m_section, ": outbound lost: ob_hello=",
                m_ob_hello_received, " ob_hb_rx=", m_ob_hb_rx_cnt,
                " ib_connected=", (m_inbound_con != nullptr),
                " ib_hello=", m_ib_hello_received);

  m_ob_hello_received = false;
  m_ob_hb_tx_cnt = 0;
  m_ob_hb_rx_cnt = 0;

  // Disable heartbeat timer if inbound is also down
  if (m_inbound_con == nullptr)
  {
    m_heartbeat_timer.setEnable(false);
  }

  // Emit after state is cleared so isActive()-based cleanup in consumers
  // sees the correct post-disconnect state.
  m_reflector->onTrunkStateChanged(m_section, peerId(), "outbound", false);

  if (!isActive()) m_partner_nodes.clear();

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
    geulog::error("trunk", "[", m_section, "] Failed to unpack trunk message header");
    return;
  }

  // Determine which connection this frame arrived on
  bool is_inbound = (con == m_inbound_con);
  bool hello_done = is_inbound ? m_ib_hello_received : m_ob_hello_received;

  // Only allow hello and heartbeat before hello exchange completes
  if (!hello_done &&
      header.type() != MsgPeerHello::TYPE &&
      header.type() != MsgPeerHeartbeat::TYPE)
  {
    geulog::warn("trunk", "[", m_section, "] Ignoring trunk message type=",
                 header.type(), " before hello");
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

  if (header.type() != MsgPeerHeartbeat::TYPE)
  {
    geulog::debug("trunk", m_section, ": rx ", (is_inbound ? "IB" : "OB"),
                  " type=", header.type(), " len=", data.size());
  }

  switch (header.type())
  {
    case MsgPeerHeartbeat::TYPE:
      handleMsgPeerHeartbeat();
      break;
    case MsgPeerHello::TYPE:
      handleMsgPeerHello(ss, is_inbound);
      break;
    case MsgPeerTalkerStart::TYPE:
      handleMsgPeerTalkerStart(ss);
      break;
    case MsgPeerTalkerStop::TYPE:
      handleMsgPeerTalkerStop(ss);
      break;
    case MsgPeerAudio::TYPE:
      handleMsgPeerAudio(ss);
      break;
    case MsgPeerFlush::TYPE:
      handleMsgPeerFlush(ss);
      break;
    case MsgPeerNodeList::TYPE:
      handleMsgPeerNodeList(ss);
      break;
    default:
      geulog::warn("trunk", "[", m_section, "] Unknown trunk message type=",
                   header.type());
      break;
  }
} /* TrunkLink::onFrameReceived */


void TrunkLink::handleMsgPeerHeartbeat(void)
{
  // rx counter already reset in onFrameReceived
} /* TrunkLink::handleMsgPeerHeartbeat */


void TrunkLink::handleMsgPeerHello(std::istream& is, bool is_inbound)
{
  // Inbound hellos are already handled by acceptInboundConnection.
  // A duplicate arriving here means the peer re-sent (e.g. TcpPrioClient
  // background reconnect) — ignore it silently.
  if (is_inbound)
  {
    geulog::debug("trunk", m_section, ": ignoring duplicate hello on inbound");
    return;
  }

  // Hello on outbound = peer's reply to our outbound hello
  MsgPeerHello msg;
  if (!msg.unpack(is))
  {
    geulog::error("trunk", "[", m_section, "] Failed to unpack MsgPeerHello");
    return;
  }

  if (msg.id().empty())
  {
    geulog::error("trunk", "[", m_section,
                  "] Peer sent empty trunk ID in MsgPeerHello");
    m_con.disconnect();
    return;
  }

  // Verify shared secret via HMAC
  if (!msg.verify(m_secret))
  {
    geulog::error("trunk", "[", m_section,
                  "] Trunk authentication failed — peer '", msg.id(),
                  "' sent invalid secret (HMAC mismatch)");
    m_con.disconnect();
    return;
  }

  m_peer_priority = msg.priority();
  m_peer_id_received = sanitizeIdent(msg.id(), 64);
  m_ob_hello_received = true;

  geulog::info("trunk", m_section, ": Trunk hello from peer '", msg.id(),
               "' local_prefix=", msg.localPrefix(),
               " priority=", m_peer_priority, " (authenticated)");
  geulog::debug("trunk", m_section, ": hello done: ob_hello=",
                m_ob_hello_received,
                " ib_connected=", (m_inbound_con != nullptr),
                " ib_hello=", m_ib_hello_received,
                " isActive=", isActive());
} /* TrunkLink::handleMsgPeerHello */


void TrunkLink::handleMsgPeerTalkerStart(std::istream& is)
{
  MsgPeerTalkerStart msg;
  if (!msg.unpack(is))
  {
    geulog::error("trunk", "[", m_section, "] Failed to unpack MsgPeerTalkerStart");
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
      geulog::info("trunk", m_section, ": TG #", local_tg,
                   " conflict — local wins (our priority=", m_priority,
                   " <= peer=", m_peer_priority, ")");
      return;
    }
    // We defer — clear local talker and accept remote
    geulog::info("trunk", m_section, ": TG #", local_tg,
                 " conflict — deferring to peer (our priority=", m_priority,
                 " > peer=", m_peer_priority, ")");
    m_yielded_tgs.insert(local_tg);
    TGHandler::instance()->setTalkerForTG(local_tg, nullptr);
    // onTalkerUpdated will fire; Reflector must not re-send TrunkTalkerStart
    // for this TG since it's in m_yielded_tgs (checked in Reflector.cpp)
  }

  m_peer_active_tgs.insert(local_tg);
  m_peer_interested_tgs[local_tg] = std::time(nullptr);
  TGHandler::instance()->setTrunkTalkerForTG(local_tg, msg.callsign());
  m_reflector->notifyExternalTrunkTalkerStart(local_tg, m_section, msg.callsign());

  // Owner-relay: if we own this TG, propagate the talker-start to every
  // other interested trunk peer so their local clients and our mesh-wide
  // audience learn about the remote talker.
  if (m_reflector->isLocalTG(local_tg))
  {
    m_reflector->forwardTrunkTalkerStartToOtherTrunks(this, local_tg,
                                                     msg.callsign());
  }
} /* TrunkLink::handleMsgPeerTalkerStart */


void TrunkLink::handleMsgPeerTalkerStop(std::istream& is)
{
  MsgPeerTalkerStop msg;
  if (!msg.unpack(is))
  {
    geulog::error("trunk", "[", m_section, "] Failed to unpack MsgPeerTalkerStop");
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

  // Owner-relay: propagate the talker-stop to every other interested peer.
  if (m_reflector->isLocalTG(local_tg))
  {
    m_reflector->forwardTrunkTalkerStopToOtherTrunks(this, local_tg);
  }
} /* TrunkLink::handleMsgPeerTalkerStop */


void TrunkLink::handleMsgPeerAudio(std::istream& is)
{
  MsgPeerAudio msg;
  if (!msg.unpack(is))
  {
    geulog::error("trunk", "[", m_section, "] Failed to unpack MsgPeerAudio");
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

  // Owner-relay: when the TG is ours, fan the audio out to every other
  // trunk peer that has interest (so mesh-wide audience hears the talker).
  if (m_reflector->isLocalTG(local_tg))
  {
    m_reflector->forwardTrunkAudioToOtherTrunks(this, local_tg, msg.audio());
  }
} /* TrunkLink::handleMsgPeerAudio */


void TrunkLink::handleMsgPeerFlush(std::istream& is)
{
  MsgPeerFlush msg;
  if (!msg.unpack(is))
  {
    geulog::error("trunk", "[", m_section, "] Failed to unpack MsgPeerFlush");
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

  // Owner-relay: when the TG is ours, fan the flush out to every other
  // trunk peer so their clients see end-of-stream as well.
  if (m_reflector->isLocalTG(local_tg))
  {
    m_reflector->forwardTrunkFlushToOtherTrunks(this, local_tg);
  }
} /* TrunkLink::handleMsgPeerFlush */


void TrunkLink::sendMsg(const ReflectorMsg& msg)
{
  if (m_paired)
  {
    // Sticky selection: try current m_sticky_ob_idx first, then advance to
    // the next live one on failure. Instant failover — no holdoff.
    if (!m_ob_cons.empty())
    {
      for (size_t tries = 0; tries < m_ob_cons.size(); ++tries)
      {
        size_t idx = (m_sticky_ob_idx + tries) % m_ob_cons.size();
        if (m_ob_cons[idx]->isConnected() && m_ob_states[idx].hello_received)
        {
          if (idx != m_sticky_ob_idx)
          {
            geulog::info("trunk", m_section, ": sticky OB switched from #",
                       m_sticky_ob_idx, " to #", idx);
            m_sticky_ob_idx = idx;
          }
          sendMsgOnPairedOutbound(idx, msg);
          return;
        }
      }
    }
    // All paired outbounds down — fall back to any inbound (same as D3)
    for (size_t i = 0; i < m_ib_cons.size(); ++i)
    {
      if (m_ib_cons[i]->isConnected() && m_ib_states[i].hello_received)
      {
        geulog::debug("trunk", m_section, ": paired tx fallback to IB#", i,
                      " type=", msg.type());
        sendMsgOnPairedInbound(i, msg);
        return;
      }
    }
    geulog::debug("trunk", m_section, ": paired tx dropped type=", msg.type(),
                  " (no active connection)");
    return;
  }

  if (isOutboundReady())
  {
    sendMsgOnOutbound(msg);
  }
  else if (isInboundReady())
  {
    geulog::debug("trunk", m_section, ": tx fallback to IB type=", msg.type());
    sendMsgOnInbound(msg);
  }
  else
  {
    geulog::debug("trunk", m_section, ": tx dropped type=", msg.type(),
                  " (no active connection)");
  }
} /* TrunkLink::sendMsg */


void TrunkLink::sendMsgOnOutbound(const ReflectorMsg& msg)
{
  ostringstream ss;
  ReflectorMsg header(msg.type());
  if (!header.pack(ss) || !msg.pack(ss))
  {
    geulog::error("trunk", "[", m_section, "] Failed to pack trunk message type=",
                  msg.type());
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
    geulog::error("trunk", "[", m_section, "] Failed to pack trunk message type=",
                  msg.type());
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
        geulog::debug("trunk", m_section, ": paired OB#", i, " heartbeat tx",
                      " hb_rx=", m_ob_states[i].hb_rx_cnt);
        sendMsgOnPairedOutbound(i, MsgPeerHeartbeat());
      }
      if (--m_ob_states[i].hb_rx_cnt == 0)
      {
        geulog::error("trunk", "[", m_section, "] Paired outbound #", i,
                      " heartbeat timeout");
        m_ob_cons[i]->disconnect();
      }
      else if (m_ob_states[i].hb_rx_cnt <= 5)
      {
        geulog::debug("trunk", m_section, ": paired OB#", i,
                      " heartbeat rx countdown: ", m_ob_states[i].hb_rx_cnt);
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
        geulog::debug("trunk", m_section, ": paired IB#", i, " heartbeat tx",
                      " hb_rx=", m_ib_states[i].hb_rx_cnt);
        sendMsgOnPairedInbound(i, MsgPeerHeartbeat());
      }
      if (--m_ib_states[i].hb_rx_cnt == 0)
      {
        geulog::error("trunk", "[", m_section, "] Paired inbound #", i,
                      " heartbeat timeout");
        m_ib_cons[i]->disconnect();
      }
      else if (m_ib_states[i].hb_rx_cnt <= 5)
      {
        geulog::debug("trunk", m_section, ": paired IB#", i,
                      " heartbeat rx countdown: ", m_ib_states[i].hb_rx_cnt);
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
      geulog::debug("trunk", m_section, ": OB heartbeat tx ob_hb_rx=",
                    m_ob_hb_rx_cnt);
      sendMsgOnOutbound(MsgPeerHeartbeat());
    }
    if (--m_ob_hb_rx_cnt == 0)
    {
      geulog::error("trunk", "[", m_section, "] Outbound heartbeat timeout");
      m_con.disconnect();
    }
    else if (m_ob_hb_rx_cnt <= 5)
    {
      geulog::debug("trunk", m_section, ": OB heartbeat rx countdown: ",
                    m_ob_hb_rx_cnt);
    }
  }

  // Inbound heartbeat
  if (m_inbound_con != nullptr && m_ib_hb_rx_cnt > 0)
  {
    if (--m_ib_hb_tx_cnt == 0)
    {
      geulog::debug("trunk", m_section, ": IB heartbeat tx ib_hb_rx=",
                    m_ib_hb_rx_cnt);
      sendMsgOnInbound(MsgPeerHeartbeat());
    }
    if (--m_ib_hb_rx_cnt == 0)
    {
      geulog::error("trunk", "[", m_section, "] Inbound heartbeat timeout");
      m_inbound_con->disconnect();
    }
    else if (m_ib_hb_rx_cnt <= 5)
    {
      geulog::debug("trunk", m_section, ": IB heartbeat rx countdown: ",
                    m_ib_hb_rx_cnt);
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
  if (!m_ob_cons[idx]->isConnected()) return;
  ostringstream ss;
  ReflectorMsg header(msg.type());
  if (!header.pack(ss) || !msg.pack(ss))
  {
    geulog::error("trunk", "[", m_section, "] Failed to pack trunk message type=",
                  msg.type(), " for paired outbound #", idx);
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
  if (!m_ib_cons[idx]->isConnected()) return;
  ostringstream ss;
  ReflectorMsg header(msg.type());
  if (!header.pack(ss) || !msg.pack(ss))
  {
    geulog::error("trunk", "[", m_section, "] Failed to pack trunk message type=",
                  msg.type(), " for paired inbound #", idx);
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
    geulog::error("trunk", "[", m_section,
                  "] Failed to unpack frame on paired inbound #", idx);
    return;
  }

  switch (header.type())
  {
    case MsgPeerHello::TYPE:
      // Duplicate hello — the hello was already processed by Reflector's
      // acceptInboundConnection path; ignore silently.
      break;

    case MsgPeerHeartbeat::TYPE:
      // hb_rx_cnt already reset above; nothing else needed
      break;

    default:
      // Dispatch data messages to shared handlers
      switch (header.type())
      {
        case MsgPeerTalkerStart::TYPE:
          handleMsgPeerTalkerStart(ss);
          break;
        case MsgPeerTalkerStop::TYPE:
          handleMsgPeerTalkerStop(ss);
          break;
        case MsgPeerAudio::TYPE:
          handleMsgPeerAudio(ss);
          break;
        case MsgPeerFlush::TYPE:
          handleMsgPeerFlush(ss);
          break;
        case MsgPeerNodeList::TYPE:
          handleMsgPeerNodeList(ss);
          break;
        default:
          geulog::warn("trunk", "[", m_section, "] Unknown trunk message type=",
                       header.type(), " on paired ib#", idx);
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
  geulog::info("trunk", m_section, ": paired inbound #", idx, " disconnected",
               " remaining=", (m_ib_cons.size() - 1));
  m_ib_cons.erase(m_ib_cons.begin() + idx);
  m_ib_states.erase(m_ib_states.begin() + idx);
  // Reflector owns the connection object — do NOT delete it here.

  if (!isActive()) m_partner_nodes.clear();
} /* TrunkLink::onPairedInboundDisconnected */


void TrunkLink::onPairedOutboundConnected(FramedTcpClient* client)
{
  size_t idx = pairedClientIndex(client);
  if (idx == SIZE_MAX) return;

  m_ob_states[idx].hello_received = false;
  m_ob_states[idx].hb_tx_cnt = HEARTBEAT_TX_CNT_RESET;
  m_ob_states[idx].hb_rx_cnt = HEARTBEAT_RX_CNT_RESET;
  m_heartbeat_timer.setEnable(true);

  geulog::info("trunk", m_section, ": paired outbound #", idx,
               " connected to ", m_peer_hosts[idx], ":", m_peer_port);
  geulog::debug("trunk", m_section, ": paired ob#", idx,
                " sending hello with priority=", m_priority);

  sendMsgOnPairedOutbound(idx,
      MsgPeerHello(m_peer_id_config, joinPrefixes(m_local_prefix),
                    m_priority, m_secret, MsgPeerHello::ROLE_PEER));
} /* TrunkLink::onPairedOutboundConnected */


void TrunkLink::onPairedOutboundDisconnected(FramedTcpClient* client,
    Async::TcpConnection* /*con*/,
    Async::TcpConnection::DisconnectReason reason)
{
  size_t idx = pairedClientIndex(client);
  if (idx == SIZE_MAX) return;

  geulog::info("trunk", m_section, ": paired outbound #", idx,
               " disconnected (",
               Async::TcpConnection::disconnectReasonStr(reason), ")");

  m_ob_states[idx].hello_received = false;
  m_ob_states[idx].hb_tx_cnt = 0;
  m_ob_states[idx].hb_rx_cnt = 0;

  // If we were stuck on this socket, advance so next send picks a live one.
  if (m_sticky_ob_idx == idx && !m_ob_cons.empty())
  {
    m_sticky_ob_idx = (idx + 1) % m_ob_cons.size();
  }

  if (!isActive()) m_partner_nodes.clear();
  // TcpPrioClient auto-reconnects; sticky may switch back here next send.
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
    geulog::error("trunk", "[", m_section,
                  "] Failed to unpack frame on paired outbound #", idx);
    return;
  }

  // Only allow hello and heartbeat before handshake completes
  if (!m_ob_states[idx].hello_received &&
      header.type() != MsgPeerHello::TYPE &&
      header.type() != MsgPeerHeartbeat::TYPE)
  {
    geulog::warn("trunk", "[", m_section, "] Ignoring paired ob#", idx,
                 " msg type=", header.type(), " before hello");
    return;
  }

  switch (header.type())
  {
    case MsgPeerHello::TYPE:
    {
      MsgPeerHello msg;
      if (!msg.unpack(ss)) return;

      if (msg.id().empty())
      {
        geulog::error("trunk", "[", m_section,
                      "] Peer sent empty ID on paired ob#", idx);
        client->disconnect();
        return;
      }

      if (!msg.verify(m_secret))
      {
        geulog::error("trunk", "[", m_section, "] HMAC failed on paired outbound #",
                      idx, " (peer='", msg.id(), "')");
        client->disconnect();
        return;
      }

      m_ob_states[idx].hello_received = true;
      // Use priority/id from whichever client completes the handshake first
      // (all twins share the same peer priority; last-writer wins is fine here)
      m_peer_priority = msg.priority();
      if (m_peer_id_received.empty())
        m_peer_id_received = sanitizeIdent(msg.id(), 64);

      geulog::info("trunk", m_section, ": paired outbound #", idx,
                   " hello received (peer='", msg.id(),
                   "' priority=", msg.priority(), " authenticated)");
      break;
    }

    case MsgPeerHeartbeat::TYPE:
      // hb_rx_cnt already reset above; nothing else needed
      break;

    default:
      // For data messages (talker start/stop, audio, flush, node-list),
      // dispatch to the shared handlers — they don't need per-client context.
      // `ss` is already positioned past the header so we can pass it directly.
      switch (header.type())
      {
        case MsgPeerTalkerStart::TYPE:
          handleMsgPeerTalkerStart(ss);
          break;
        case MsgPeerTalkerStop::TYPE:
          handleMsgPeerTalkerStop(ss);
          break;
        case MsgPeerAudio::TYPE:
          handleMsgPeerAudio(ss);
          break;
        case MsgPeerFlush::TYPE:
          handleMsgPeerFlush(ss);
          break;
        case MsgPeerNodeList::TYPE:
          handleMsgPeerNodeList(ss);
          break;
        default:
          geulog::warn("trunk", "[", m_section, "] Unknown trunk message type=",
                       header.type(), " on paired ob#", idx);
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

  geulog::info("trunk", m_section, ": Reloaded filters",
               (m_blacklist_filter.empty() ? "" :
                " blacklist=" + m_blacklist_filter.toString()),
               (m_allow_filter.empty()     ? "" :
                " allow=" + m_allow_filter.toString()),
               " tg_map_entries=", m_tg_map_in.size());
} /* TrunkLink::reloadConfig */


void TrunkLink::sendNodeList(
    const std::vector<MsgPeerNodeList::NodeEntry>& nodes)
{
  if (!isActive()) return;
  sendMsg(MsgPeerNodeList(nodes));
} /* TrunkLink::sendNodeList */


void TrunkLink::handleMsgPeerNodeList(std::istream& is)
{
  MsgPeerNodeList msg;
  if (!msg.unpack(is))
  {
    geulog::error("trunk", "[", m_section, "] Failed to unpack MsgPeerNodeList");
    return;
  }

  // Sanitize every entry before handing off. Untrusted strings from peer
  // reflectors flow into Redis key names, MQTT payloads and log output;
  // keep them byte-bounded and free of control / delimiter characters.
  std::vector<MsgPeerNodeList::NodeEntry> sanitized;
  sanitized.reserve(msg.nodes().size());
  unsigned dropped = 0;
  for (const auto& n : msg.nodes())
  {
    MsgPeerNodeList::NodeEntry e;
    e.callsign = sanitizeIdent(n.callsign, 32);
    if (e.callsign.empty())
    {
      ++dropped;
      continue;
    }
    e.tg       = n.tg;
    e.qth_name = sanitizeText(n.qth_name, 64);

    // Lat/lon: reject non-finite; drop out-of-range values by zeroing
    // them (keep the callsign, lose the coordinates). Downstream consumers
    // already treat (0,0) as "no location".
    if (std::isfinite(n.lat) && std::isfinite(n.lon) &&
        n.lat >= -90.0f && n.lat <= 90.0f &&
        n.lon >= -180.0f && n.lon <= 180.0f)
    {
      e.lat = n.lat;
      e.lon = n.lon;
    }
    else
    {
      e.lat = 0.0f;
      e.lon = 0.0f;
    }
    e.status = n.status;
    sanitizeJsonStrings(e.status);
    // Recipient-relative sat_id from the peer. Pass through as-is after
    // bounded sanitisation: empty = "on the peer itself", non-empty =
    // "on a satellite attached to the peer".
    e.sat_id = sanitizeIdent(n.sat_id, 64);
    sanitized.push_back(std::move(e));
  }
  if (dropped > 0)
  {
    geulog::warn("trunk", "[", m_section, "] dropped ", dropped,
                 " node list entrie(s) with empty/invalid callsign after sanitization");
  }

  m_reflector->onPeerNodeList(peerId(), sanitized);
  m_partner_nodes = std::move(sanitized);
} /* TrunkLink::handleMsgPeerNodeList */


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
