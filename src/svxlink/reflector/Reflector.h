/**
@file	 Reflector.h
@brief   The main reflector class
@author  Tobias Blomberg / SM0SVX
@date	 2017-02-11

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

#ifndef REFLECTOR_INCLUDED
#define REFLECTOR_INCLUDED


/****************************************************************************
 *
 * System Includes
 *
 ****************************************************************************/

#include <sigc++/sigc++.h>
#include <sys/time.h>
#include <set>
#include <vector>
#include <string>
#include <json/json.h>


/****************************************************************************
 *
 * Project Includes
 *
 ****************************************************************************/

#include <AsyncTcpServer.h>
#include <AsyncFramedTcpConnection.h>
#include <AsyncTimer.h>
#include <AsyncAtTimer.h>
#include <AsyncHttpServerConnection.h>
#include <AsyncExec.h>


/****************************************************************************
 *
 * Local Includes
 *
 ****************************************************************************/

#include "ProtoVer.h"
#include "ReflectorClient.h"
#include "TrunkLink.h"
#include "ReflectorMsg.h"
#include "SatelliteLink.h"
#include "SatelliteClient.h"
#include "MqttPublisher.h"


/****************************************************************************
 *
 * Forward declarations
 *
 ****************************************************************************/

namespace Async
{
  class EncryptedUdpSocket;
  class Config;
  class Pty;
};

class ReflectorMsg;
class ReflectorUdpMsg;
class RedisStore;
class TwinLink;


/****************************************************************************
 *
 * Forward declarations of classes inside of the declared namespace
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Defines & typedefs
 *
 ****************************************************************************/

/**
 * @brief Structure to hold certificate or CSR information
 *
 * - Signed certificates (is_signed=true, has valid_until/not_after)
 * - Pending CSRs (is_signed=false, has received_time)
 */
struct CertInfo
{
  std::string callsign;            // Common Name
  std::vector<std::string> emails; // Email addresses from SAN
  bool is_signed;                  // true=signed cert, false=pending CSR

    // For signed certificates:
  std::string valid_until;         // Human-readable expiry date
  time_t not_after;                // Unix timestamp for expiry (0 if pending)

    // For pending CSRs:
  time_t received_time;            // Unix timestamp when CSR received (0 if cert)

  CertInfo() : is_signed(false), not_after(0), received_time(0) {}
};



/****************************************************************************
 *
 * Exported Global Variables
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Class definitions
 *
 ****************************************************************************/

/**
@brief	The main reflector class
@author Tobias Blomberg / SM0SVX
@date   2017-02-11

This is the main class for the reflector. It handles all network traffic and
the dispatching of incoming messages to the correct ReflectorClient object.
*/
class Reflector : public sigc::trackable
{
  public:
    static time_t timeToRenewCert(const Async::SslX509& cert);

    /**
     * @brief 	Default constructor
     */
    Reflector(void);

    /**
     * @brief 	Destructor
     */
    ~Reflector(void);

    /**
     * @brief 	Initialize the reflector
     * @param 	cfg A previously initialized configuration object
     * @return	Return \em true on success or else \em false
     */
    bool initialize(Async::Config &cfg);

    /**
     * @brief   Return a list of all connected nodes
     * @param   nodes The vector to return the result in
     *
     * This function is used to get a list of the callsigns of all connected
     * nodes.
     */
    void nodeList(std::vector<std::string>& nodes) const;

    /**
     * @brief   Broadcast a TCP message to connected clients
     * @param   msg The message to broadcast
     * @param   filter The client filter to apply
     *
     * This function is used to broadcast a message to all connected clients,
     * possibly applying a client filter.  The message is not really a IP
     * broadcast but rather unicast to all connected clients.
     */
    void broadcastMsg(const ReflectorMsg& msg,
        const ReflectorClient::Filter& filter=ReflectorClient::NoFilter());

    /**
     * @brief   Send a UDP datagram to the specificed ReflectorClient
     * @param   client The client to the send datagram to
     * @param   buf The payload to send
     * @param   count The number of bytes in the payload
     * @return  Returns \em true on success or else \em false
     */
    bool sendUdpDatagram(ReflectorClient *client, const ReflectorUdpMsg& msg);

    void broadcastUdpMsg(const ReflectorUdpMsg& msg,
        const ReflectorClient::Filter& filter=ReflectorClient::NoFilter());

    /**
     * @brief   Get the TG for protocol V1 clients
     * @return  Returns the TG used for protocol V1 clients
     */
    uint32_t tgForV1Clients(void) { return m_tg_for_v1_clients; }

    /**
     * @brief   Request QSY to another talk group
     * @param   tg The talk group to QSY to
     */
    void requestQsy(ReflectorClient *client, uint32_t tg);

    Async::EncryptedUdpSocket* udpSocket(void) const { return m_udp_sock; }

    uint32_t randomQsyLo(void) const { return m_random_qsy_lo; }
    uint32_t randomQsyHi(void) const { return m_random_qsy_hi; }

    Async::SslCertSigningReq loadClientPendingCsr(const std::string& callsign);
    Async::SslCertSigningReq loadClientCsr(const std::string& callsign);
    bool renewedClientCert(Async::SslX509& cert);
    bool signClientCert(Async::SslX509& cert, const std::string& ca_op);
    Async::SslX509 signClientCsr(const std::string& cn);
    Async::SslX509 loadClientCertificate(const std::string& callsign);

    size_t caSize(void) const { return m_ca_size; }
    const std::vector<uint8_t>& caDigest(void) const { return m_ca_md; }
    const std::vector<uint8_t>& caSignature(void) const { return m_ca_sig; }
    std::string clientCertPem(const std::string& callsign) const;
    std::string caBundlePem(void) const;
    std::string issuingCertPem(void) const;
    bool callsignOk(const std::string& callsign, bool verbose=true) const;
    bool reqEmailOk(const Async::SslCertSigningReq& req) const;
    bool emailOk(const std::string& email) const;
    std::string checkCsr(const Async::SslCertSigningReq& req);
    Async::SslX509 csrReceived(Async::SslCertSigningReq& req);

    Json::Value& clientStatus(const std::string& callsign);

    bool isClusterTG(uint32_t tg) const { return m_cluster_tgs.count(tg) > 0; }
    bool isSatelliteMode(void) const { return m_is_satellite; }

    // True iff this reflector owns the TG (i.e., our local prefix is the
    // longest matching prefix across all known prefixes in the mesh).
    // Used to gate owner-relay fanout of trunk-received audio.
    bool isLocalTG(uint32_t tg) const;

    // Owner-relay fanout: when we receive trunk traffic for a TG we own,
    // forward to every other trunk peer except `src`. Each peer's TrunkLink
    // applies its own shared/cluster/interest filter, so links with no
    // reason to care drop the forward.
    void forwardTrunkAudioToOtherTrunks(const TrunkLink* src, uint32_t tg,
                                         const std::vector<uint8_t>& audio);
    void forwardTrunkFlushToOtherTrunks(const TrunkLink* src, uint32_t tg);
    void forwardTrunkTalkerStartToOtherTrunks(const TrunkLink* src,
                                               uint32_t tg,
                                               const std::string& callsign);
    void forwardTrunkTalkerStopToOtherTrunks(const TrunkLink* src,
                                              uint32_t tg);

    RedisStore* redisStore(void) const { return m_redis; }
    MqttPublisher* mqtt(void) const { return m_mqtt; }

    // Callbacks for SatelliteLink to forward satellite events to trunk peers
    void forwardSatelliteAudioToTrunks(uint32_t tg,
                                        const std::string& callsign);
    void forwardSatelliteStopToTrunks(uint32_t tg);
    void forwardSatelliteRawAudioToTrunks(uint32_t tg,
                                           const std::vector<uint8_t>& audio);
    void forwardSatelliteFlushToTrunks(uint32_t tg);
    void forwardAudioToSatellitesExcept(SatelliteLink* except, uint32_t tg,
                                         const std::vector<uint8_t>& audio);
    void forwardFlushToSatellitesExcept(SatelliteLink* except, uint32_t tg);

    void publishRxUpdate(ReflectorClient* client);
    // Push the rich per-client status blob (rx, monitoredTGs, qth, ...)
    // to Redis. Cheap no-op if Redis is not configured.
    void publishClientStatus(ReflectorClient* client);

    // Per-client liveness fanout to satellite and twin peers. Iterates
    // m_satellite_con_map, m_satellite_client, and m_twin_link. Trunk
    // peers are intentionally NOT fanned out (deferred per design spec).
    // Bodies are stubs in this commit and become real iteration in Task 8.
    void fanoutClientConnected(const std::string& callsign, uint32_t tg,
                               const std::string& ip);
    void fanoutClientDisconnected(const std::string& callsign);
    void fanoutClientRx(const std::string& callsign,
                        const Json::Value& rx_json);
    void fanoutClientStatus(const std::string& callsign,
                            const Json::Value& status_json);
    // Reflector-wide live:meta hash (mode, version, prefixes, ports,
    // cluster TGs, satellite-server stats). Event-driven write.
    void publishMetaToRedis(void);
    // Per-satellite live:satellite:<id> snapshot. Triggered on satellite
    // hello + active-TG changes.
    void publishSatelliteStatusToRedis(SatelliteLink* link);
    // Per-trunk live:trunk:<section> `status` field with full
    // TrunkLink::statusJson(). Triggered on trunk state-change events.
    void publishTrunkStatusToRedis(TrunkLink* link);
    void onClientAuthenticated(const std::string& callsign, uint32_t tg,
                               const std::string& ip);
    void notifyExternalTrunkTalkerStart(uint32_t tg,
                                         const std::string& peer_id,
                                         const std::string& callsign);
    void notifyExternalTrunkTalkerStop(uint32_t tg,
                                        const std::string& peer_id);
    void onTrunkStateChanged(const std::string& section,
                             const std::string& peer_id,
                             const std::string& direction, bool up,
                             const std::string& host = "",
                             uint16_t port = 0);

    // Triggered when a local client logs in/out or changes TG. Schedules
    // a debounced node-list emission to all trunk peers and to MQTT.
    void scheduleNodeListUpdate(void);
    // Called by TrunkLink when a peer sends us its node list.
    void onPeerNodeList(const std::string& peer_id,
                        const std::vector<MsgPeerNodeList::NodeEntry>& nodes);

  protected:

  private:
    typedef std::map<Async::FramedTcpConnection*,
                     ReflectorClient*> ReflectorClientConMap;
    typedef Async::TcpServer<Async::FramedTcpConnection> FramedTcpServer;
    using HttpServer = Async::TcpServer<Async::HttpServerConnection>;

    static constexpr unsigned ROOT_CA_VALIDITY_DAYS     = 25*365;
    static constexpr unsigned ISSUING_CA_VALIDITY_DAYS  = 4*90;
    static constexpr unsigned CERT_VALIDITY_DAYS        = 90;
    static constexpr int      CERT_VALIDITY_OFFSET_DAYS = -1;

    FramedTcpServer*            m_srv;
    Async::EncryptedUdpSocket*  m_udp_sock;
    ReflectorClientConMap       m_client_con_map;
    Async::Config*              m_cfg;
    uint32_t                    m_tg_for_v1_clients;
    uint32_t                    m_random_qsy_lo;
    uint32_t                    m_random_qsy_hi;
    uint32_t                    m_random_qsy_tg;
    HttpServer*                 m_http_server;
    Async::Pty*                 m_cmd_pty;
    Async::SslContext           m_ssl_ctx;
    std::string                 m_keys_dir;
    std::string                 m_pending_csrs_dir;
    std::string                 m_csrs_dir;
    std::string                 m_certs_dir;
    UdpCipher::AAD              m_aad;
    Async::SslKeypair           m_ca_pkey;
    Async::SslX509              m_ca_cert;
    Async::SslKeypair           m_issue_ca_pkey;
    Async::SslX509              m_issue_ca_cert;
    std::string                 m_pki_dir;
    std::string                 m_ca_bundle_file;
    std::string                 m_crtfile;
    Async::AtTimer              m_renew_cert_timer;
    Async::AtTimer              m_renew_issue_ca_cert_timer;
    size_t                      m_ca_size = 0;
    std::vector<uint8_t>        m_ca_md;
    std::vector<uint8_t>        m_ca_sig;
    std::string                 m_accept_cert_email;
    Json::Value                 m_status;

    std::vector<TrunkLink*>     m_trunk_links;
    // Prefix bookkeeping for owner-relay decisions. m_local_prefixes holds
    // prefixes owned by this reflector (from GLOBAL/LOCAL_PREFIX);
    // m_all_prefixes is local + every peer's REMOTE_PREFIX. Populated in
    // initTrunkLinks() and refreshed by addTrunkLink/removeTrunkLink.
    std::vector<std::string>    m_local_prefixes;
    std::vector<std::string>    m_all_prefixes;
    // Tracks last-seen callsign set per peer_id so we can DEL dropped
    // callsigns from Redis when a peer's node list shrinks.
    std::map<std::string, std::set<std::string>> m_peer_node_cache;
    std::set<std::string>       m_redis_trunk_sections; // sections loaded from Redis
    std::set<uint32_t>          m_cluster_tgs;
    Async::Timer                m_nodelist_timer;
    static const size_t TRUNK_MAX_PENDING_CONS = 5;

    FramedTcpServer*            m_trunk_srv = nullptr;
    // Inbound trunk connections waiting for MsgPeerHello identification
    std::map<Async::FramedTcpConnection*, Async::Timer*> m_trunk_pending_cons;
    // Handed-off inbound trunk connections mapped to their TrunkLink
    std::map<Async::FramedTcpConnection*, TrunkLink*>    m_trunk_inbound_map;

    // Twin HA-pair support
    TwinLink*                                            m_twin_link = nullptr;
    Async::TcpServer<Async::FramedTcpConnection>*        m_twin_srv = nullptr;
    uint16_t                                             m_twin_listen_port = 5304;
    // Pending inbound twin connections awaiting MsgPeerHello
    std::map<Async::FramedTcpConnection*, Async::Timer*> m_twin_pending_cons;

    // Satellite support
    bool                        m_is_satellite = false;
    SatelliteClient*            m_satellite_client = nullptr;
    FramedTcpServer*            m_sat_srv = nullptr;
    std::string                 m_satellite_secret;
    std::map<Async::FramedTcpConnection*, SatelliteLink*> m_satellite_con_map;
    std::vector<SatelliteLink*>  m_sat_cleanup_pending;
    Async::Timer                 m_sat_cleanup_timer;

    // MQTT publishing
    MqttPublisher*              m_mqtt = nullptr;
    RedisStore*                 m_redis = nullptr;
    Async::Timer                m_mqtt_status_timer;

    Reflector(const Reflector&);
    Reflector& operator=(const Reflector&);
    void clientConnected(Async::FramedTcpConnection *con);
    void clientDisconnected(Async::FramedTcpConnection *con,
                            Async::FramedTcpConnection::DisconnectReason reason);
    bool udpCipherDataReceived(const Async::IpAddress& addr, uint16_t port,
                               void *buf, int count);
    void udpDatagramReceived(const Async::IpAddress& addr, uint16_t port,
                             void* aad, void *buf, int count);
    void onTalkerUpdated(uint32_t tg, ReflectorClient* old_talker,
                         ReflectorClient *new_talker);
    void httpRequestReceived(Async::HttpServerConnection *con,
                             Async::HttpServerConnection::Request& req);
    void httpClientConnected(Async::HttpServerConnection *con);
    void httpClientDisconnected(Async::HttpServerConnection *con,
        Async::HttpServerConnection::DisconnectReason reason);
    void onRequestAutoQsy(uint32_t from_tg);
    uint32_t nextRandomQsyTg(void);
    void ctrlPtyDataReceived(const void *buf, size_t count);
    void cfgUpdated(const std::string& section, const std::string& tag);
    void onTrunkTalkerUpdated(uint32_t tg, std::string old_cs,
                              std::string new_cs, std::string peer_id);
    void onRedisConfigChanged(std::string scope);
    void reloadClusterTgs(void);
    bool addTrunkLink(const std::string& section);
    bool removeTrunkLink(const std::string& section);
    std::vector<std::string> collectAllTrunkPrefixes(void) const;
    void refreshStatus(void);
    void initTrunkLinks(void);
    void initTrunkServer(void);
    void initTwinLink(void);
    void initTwinServer(void);
    void twinClientConnected(Async::FramedTcpConnection* con);
    void twinClientDisconnected(Async::FramedTcpConnection* con,
        Async::FramedTcpConnection::DisconnectReason reason);
    void twinPendingFrameReceived(Async::FramedTcpConnection* con,
                                   std::vector<uint8_t>& data);
    void twinPendingTimeout(Async::Timer* t);
    void trunkClientConnected(Async::FramedTcpConnection* con);
    void trunkClientDisconnected(Async::FramedTcpConnection* con,
        Async::FramedTcpConnection::DisconnectReason reason);
    void trunkPendingFrameReceived(Async::FramedTcpConnection* con,
                                    std::vector<uint8_t>& data);
    void trunkPendingTimeout(Async::Timer* t);
    void initSatelliteServer(void);
    void satelliteConnected(Async::FramedTcpConnection* con);
    void satelliteDisconnected(Async::FramedTcpConnection* con,
        Async::FramedTcpConnection::DisconnectReason reason);
    void onSatelliteLinkFailed(SatelliteLink* link);
    void processSatelliteCleanup(Async::Timer* t);
    void sendNodeListToAllPeers(void);
    bool loadCertificateFiles(void);
    bool loadServerCertificateFiles(void);
    bool generateKeyFile(Async::SslKeypair& pkey, const std::string& keyfile);
    bool loadRootCAFiles(void);
    bool loadSigningCAFiles(void);
    bool onVerifyPeer(Async::TcpConnection *con, bool preverify_ok,
                      X509_STORE_CTX *x509_store_ctx);
    bool buildPath(const std::string& sec, const std::string& tag,
                   const std::string& defdir, std::string& defpath);
    bool removeClientCertFiles(const std::string& cn);
    void runCAHook(const Async::Exec::Environment& env);
    std::vector<CertInfo> getAllCerts(void);
    std::vector<CertInfo> getAllPendingCSRs(void);
    std::string formatCerts(bool signedCerts=true, bool pendingCerts=true);
};  /* class Reflector */


#endif /* REFLECTOR_INCLUDED */



/*
 * This file has not been truncated
 */
